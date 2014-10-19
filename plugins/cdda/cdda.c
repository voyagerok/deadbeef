/*
    CD audio plugin for DeaDBeeF
    Copyright (C) 2009 Viktor Semykin <thesame.ml@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* screwed/maintained by Alexey Yakovenko <waker@users.sourceforge.net> */

#ifdef HAVE_CONFIG_H
    #include "../../config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#if HAVE_SYS_CDEFS_H
    #include <sys/cdefs.h>
#endif
#if HAVE_SYS_SYSLIMITS_H
    #include <sys/syslimits.h>
#endif

#if USE_PARANOIA
    #include <cdio/paranoia.h>
    #include <cdio/cdda.h>
#endif
#include <cdio/cdio.h>
#include <cdio/cdtext.h>
#include <cddb/cddb.h>

#include "../../deadbeef.h"

//#define trace(...) { fprintf (stderr, __VA_ARGS__); }
#define trace(fmt,...)

#define DEFAULT_SERVER "freedb.org"
#define DEFAULT_PORT 888
#define DEFAULT_USE_CDDB 1
#define DEFAULT_PROTOCOL 1
#define DEFAULT_PREFER_CDTEXT 1

#define SECTORSIZE CDIO_CD_FRAMESIZE_RAW //2352
#define SAMPLESIZE 4 //bytes

#define CDDB_CATEGORY_SIZE 12
#define CDDB_DISCID_SIZE 10
#define MAX_CDDB_DISCS 10
#define MAX_CDDB_MENU 80

#define CDDB_DISCID_TAG ":CDDB_DISCID"
#define CDDB_IDS_TAG ":CDDB IDs"

static DB_decoder_t plugin;
static DB_functions_t *deadbeef;

typedef struct {
    DB_fileinfo_t info;
    uint32_t hints;
    CdIo_t *cdio;
#if USE_PARANOIA
    cdrom_paranoia_t *paranoia;
#endif
    lsn_t first_sector;
    lsn_t current_sector;
    lsn_t last_sector;
    uint8_t buffer[SECTORSIZE];
    uint8_t *tail;
    size_t tail_length;
} cdda_info_t;

struct cddb_thread_params
{
    DB_playItem_t **items;
    CdIo_t *cdio;
};

typedef void (* selected_callback)(DB_playItem_t *it, va_list va);

static inline int
min (int a, int b) {
    return a < b ? a : b;
}

static void
set_disc_lengths(CdIo_t *cdio, cddb_disc_t *disc)
{
    const lba_t leadout_lba = cdio_get_track_lba(cdio, CDIO_CDROM_LEADOUT_TRACK);
    cddb_disc_set_length(disc, leadout_lba / CDIO_CD_FRAMES_PER_SEC);
    const track_t first_track = cdio_get_first_track_num(cdio);
    const track_t last_track = first_track + cdio_get_num_tracks(cdio);
    for (track_t i = first_track; i < last_track; i++)
    {
        const lba_t offset = cdio_get_track_lba(cdio, i);
        cddb_track_t *track = cddb_track_new();
        cddb_track_set_frame_offset(track, offset);
        cddb_disc_add_track(disc, track);
    }
}

static unsigned long
calc_discid(CdIo_t *cdio)
{
    cddb_disc_t *disc = cddb_disc_new();
    if (!disc)
    {
        return 0;
    }

    set_disc_lengths(cdio, disc);
    cddb_disc_calc_discid(disc);
    const unsigned long discid = cddb_disc_get_discid(disc);

    cddb_disc_destroy(disc);
    return discid;
}

static DB_fileinfo_t *
cda_open (uint32_t hints)
{
    cdda_info_t *info = calloc(1, sizeof (cdda_info_t));
    if (info)
    {
        info->hints = hints;
        info->tail = info->buffer;
        info->tail_length = 0;
        info->info.plugin = &plugin;
        info->info.fmt.bps = 16;
        info->info.fmt.channels = 2;
        info->info.fmt.samplerate = 44100;
        info->info.fmt.channelmask = DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT;
        info->info.readpos = 0;
    }
    return &info->info;
}

static int
cda_init (DB_fileinfo_t *_info, DB_playItem_t *it)
{
    cdda_info_t *info = (cdda_info_t *)_info;
    char location[PATH_MAX];
    deadbeef->pl_get_meta(it, ":URI", location, sizeof(location));
    trace ("cdda: init %s\n", location);

    char *nr = strchr(location, '#');
    if (!nr) {
        trace ("cdda: bad name: %s\n", location);
        return -1;
    }
    *nr = '\0';

    const char *fname = (*location) ? location : NULL; //NULL if empty string; means physical CD drive
    info->cdio = cdio_open(fname, DRIVER_UNKNOWN);
    if  (!info->cdio)
    {
        trace ("cdda: Could not open CD\n");
        return -1;
    }

    const int track_nr = atoi(nr+1);
    if (cdio_get_track_format(info->cdio, track_nr) != TRACK_FORMAT_AUDIO)
    {
        trace ("cdda: Not an audio track (%d)\n", track_nr);
        return -1;
    }

    const unsigned long discid = calc_discid(info->cdio);
    if (!discid) {
        trace ("cdda: no discid found\n");
        return -1;
    }

    deadbeef->pl_lock();
    const char *discid_hex = deadbeef->pl_find_meta(it, CDDB_DISCID_TAG);
    const unsigned long trk_discid = discid_hex ? strtoul(discid_hex, NULL, 16) : 0;
    deadbeef->pl_unlock();
    if (trk_discid != discid) {
        trace ("cdda: the track belongs to another disc, skipped\n");
        return -1;
    }

    trace("cdio nchannels (should always be 2 for an audio track): %d\n", cdio_get_track_channels (info->cdio, track_nr));
    info->first_sector = cdio_get_track_lsn(info->cdio, track_nr);
    info->current_sector = info->first_sector;
    info->last_sector = info->first_sector + cdio_get_track_sec_count(info->cdio, track_nr) - 1;
    cdio_set_speed(info->cdio, info->hints&DDB_DECODER_HINT_NEED_BITRATE ? 4 : -1);
#if USE_PARANOIA
    cdrom_drive_t *cdrom = cdio_cddap_identify_cdio(info->cdio, CDDA_MESSAGE_FORGETIT, NULL);
    cdio_cddap_open(cdrom);
    info->paranoia = cdio_paranoia_init(cdrom);
    const int no_paranoia = info->hints&DDB_DECODER_HINT_NEED_BITRATE || !deadbeef->conf_get_int("cdda.paranoia", 1);
    paranoia_modeset(info->paranoia, no_paranoia ? PARANOIA_MODE_DISABLE : PARANOIA_MODE_FULL^PARANOIA_MODE_NEVERSKIP);
    paranoia_seek(info->paranoia, info->first_sector, SEEK_SET);
#endif
    return 0;
}

#if USE_PARANOIA
static void
paranoia_callback(long bytes, paranoia_cb_mode_t mode)
{
    if (mode > 1 && mode != 9)
        fprintf(stderr, "%ld %d\n", bytes, mode);
}
#endif

static const char *
read_sector(cdda_info_t *info)
{
#if USE_PARANOIA
    const int16_t *p_readbuf = cdio_paranoia_read(info->paranoia, NULL);
    if (p_readbuf)
    {
        info->current_sector++;
        return (char *)p_readbuf;
    }
#else
    if (!cdio_read_audio_sector(info->cdio, info->buffer, info->current_sector))
    {
        info->current_sector++;
        return info->buffer;
    }
#endif
    return NULL;
}

static int
cda_read (DB_fileinfo_t *_info, char *bytes, int size)
{
    cdda_info_t *info = (cdda_info_t *)_info;
    char *fill = bytes;
    const char *high_water = bytes + size;

    if (info->tail_length >= size)
    {
        memcpy(fill, info->tail, size);
        info->tail += size;
        fill += size;
        info->tail_length -= size;
    }
    else if (info->tail_length > 0)
    {
        memcpy(fill, info->tail, info->tail_length);
        fill += info->tail_length;
        info->tail_length = 0;
    }

    while (fill < high_water && info->current_sector <= info->last_sector)
    {
        const char *p_readbuf = read_sector(info);
        if (!p_readbuf)
        {
            trace("cda_read: read_sector failed\n");
            return -1;
        }

        if (fill+SECTORSIZE <= high_water)
        {
            memcpy(fill, p_readbuf, SECTORSIZE);
            fill += SECTORSIZE;
        }
        else
        {
            const size_t bytes_left = high_water - fill;
            memcpy(fill, p_readbuf, bytes_left);
            fill += bytes_left;
            info->tail_length = SECTORSIZE - bytes_left;
            info->tail = info->buffer + bytes_left;
#if USE_PARANOIA
            memcpy(info->tail, p_readbuf+bytes_left, info->tail_length);
#endif
        }
    }

//    trace ("requested: %d, return: %d\n", size, fill-bytes);
    _info->readpos = (float)info->current_sector * SECTORSIZE / SAMPLESIZE / _info->fmt.samplerate;
    return fill - bytes;
}

static void
cda_free (DB_fileinfo_t *_info)
{
    if (_info) {
        cdda_info_t *info = (cdda_info_t *)_info;
        if (info->cdio) {
            cdio_destroy (info->cdio);
        }
#if USE_PARANOIA
        if (info->paranoia) {
            cdio_paranoia_free(info->paranoia);
        }
#endif
        free (_info);
    }
}

static int
cda_seek_sample (DB_fileinfo_t *_info, int sample)
{
    trace("cda_seek_sample %d\n", sample);
    cdda_info_t *info = (cdda_info_t *)_info;
    const int sector = sample * SAMPLESIZE / SECTORSIZE + info->first_sector;
    const int offset = sample * SAMPLESIZE % SECTORSIZE;

#if USE_PARANOIA
    paranoia_seek(info->paranoia, sector, SEEK_SET);
#endif
    info->current_sector = sector;
    const char *p_readbuf = read_sector(info);
    if (!p_readbuf)
        return -1;

    info->tail = info->buffer + offset;
    info->tail_length = SECTORSIZE - offset;
#if USE_PARANOIA
    memcpy(info->tail, p_readbuf+offset, info->tail_length);
#endif
    _info->readpos = (float)sample / _info->fmt.samplerate;

    return 0;
}

static int
cda_seek (DB_fileinfo_t *_info, float sec)
{
    return cda_seek_sample (_info, sec * _info->fmt.samplerate);
}

static cddb_conn_t *
new_cddb_connection(void)
{
    cddb_conn_t *conn = cddb_new();
    if (conn)
    {
        deadbeef->conf_lock ();
        cddb_set_server_name (conn, deadbeef->conf_get_str_fast ("cdda.freedb.host", DEFAULT_SERVER));
        cddb_set_server_port (conn, deadbeef->conf_get_int ("cdda.freedb.port", DEFAULT_PORT));
        if (!deadbeef->conf_get_int ("cdda.protocol", DEFAULT_PROTOCOL))
        {
            cddb_http_enable (conn);
            if (deadbeef->conf_get_int ("network.proxy", 0))
            {
                cddb_set_server_port(conn, deadbeef->conf_get_int ("network.proxy.port", 8080));
                cddb_set_server_name(conn, deadbeef->conf_get_str_fast ("network.proxy.address", ""));
            }
        }
        deadbeef->conf_unlock ();
    }

    return conn;
}

static int
resolve_disc (CdIo_t *cdio, cddb_disc_t* disc, char *disc_list)
{
    cddb_conn_t *conn = new_cddb_connection();
    if (!conn)
    {
        return 0;
    }

    set_disc_lengths(cdio, disc);
    cddb_disc_t *temp_disc = cddb_disc_clone(disc);
    cddb_disc_t *query_disc = disc;

    cddb_cache_disable(conn);
    const int matches = cddb_query(conn, query_disc);

    cddb_cache_enable(conn);
    size_t discs_read = 0;
    disc_list[0] = '\0';
    for (int i = 0; i < matches; i++)
    {
        if (cddb_read(conn, query_disc) && discs_read < MAX_CDDB_DISCS)
        {
            discs_read++;
            char temp_string[CDDB_CATEGORY_SIZE + CDDB_DISCID_SIZE + 1];
            sprintf(temp_string, ",%s/%08x", cddb_disc_get_category_str(query_disc), cddb_disc_get_discid(query_disc));
            strcat(disc_list, temp_string);
            query_disc = temp_disc;
        }
        cddb_query_next(conn, query_disc);
    }

    cddb_disc_destroy(temp_disc);
    cddb_destroy(conn);
    return discs_read;
}

static DB_playItem_t *
insert_single_track (CdIo_t* cdio, ddb_playlist_t *plt, DB_playItem_t *after, const char* file, int track_nr, int discid)
{
    char tmp[file ? strlen (file) + 20 : 20];
    if (file)
        snprintf (tmp, sizeof (tmp), "%s#%d.cda", file, track_nr);
    else
        snprintf (tmp, sizeof (tmp), "#%d.cda", track_nr);

    if (TRACK_FORMAT_AUDIO != cdio_get_track_format (cdio, track_nr))
    {
        trace ("Not an audio track (%d)\n", track_nr);
        return NULL;
    }


    DB_playItem_t *it = deadbeef->pl_item_alloc_init (tmp, plugin.plugin.id);
    if (it)
    {
        deadbeef->pl_add_meta (it, ":FILETYPE", "cdda");

        const float sector_count = cdio_get_track_sec_count (cdio, track_nr);
        deadbeef->plt_set_item_duration (plt, it, sector_count / 75);

        snprintf (tmp, sizeof (tmp), "CD Track %02d", track_nr);
        deadbeef->pl_add_meta (it, "title", tmp);

        char discid_string[10];
        sprintf(discid_string, "%08x", discid);
        deadbeef->pl_add_meta (it, CDDB_DISCID_TAG, discid_string);

        it = deadbeef->plt_insert_item (plt, after, it);
    }

    return it;
}

static void
cleanup_thread_params (struct cddb_thread_params *params)
{
    int i;
    for (i = 0; params->items[i]; i++)
        deadbeef->pl_item_unref (params->items[i]);
    cdio_destroy (params->cdio);
    free(params->items);
    free (params);
}

static void
write_metadata(DB_playItem_t *item, const cddb_disc_t *disc, const char *num_tracks,
               const int track_index, const char *artist, const char *disc_title, const char *genre, const unsigned int year)
{
    cddb_track_t *track = cddb_disc_get_track(disc, track_index);
    trace("track %d, title=%s\n", track_index, cddb_track_get_title(track));

    deadbeef->pl_delete_all_meta(item);
    deadbeef->pl_add_meta(item, "album", disc_title);
    deadbeef->pl_add_meta(item, "genre", genre);
    if (year) {
        deadbeef->pl_set_meta_int(item, "year", year);
    }
    deadbeef->pl_add_meta(item, "numtracks", num_tracks);
    char tmp[5];
    snprintf(tmp, sizeof(tmp), "%02d", track_index+1);
    deadbeef->pl_add_meta(item, "track", tmp);
    deadbeef->pl_add_meta(item, "artist", artist);
    deadbeef->pl_add_meta(item, "title", cddb_track_get_title(track));

    ddb_event_track_t *ev = (ddb_event_track_t *)deadbeef->event_alloc(DB_EV_TRACKINFOCHANGED);
    ev->track = item;
    if (ev->track) {
        deadbeef->pl_item_ref(ev->track);
    }
    deadbeef->event_send((ddb_event_t *)ev, 0, 0);
}

static void
cddb_thread (void *params_void)
{
    struct cddb_thread_params *params = (struct cddb_thread_params *)params_void;
    DB_playItem_t **items = params->items;

    trace ("calling resolve_disc\n");
    char disc_list[(CDDB_CATEGORY_SIZE + CDDB_DISCID_SIZE + 1) * MAX_CDDB_DISCS];
    cddb_disc_t *disc = cddb_disc_new();
    const int num_discs = resolve_disc (params->cdio, disc, disc_list);
    if (num_discs <= 0)
    {
        trace ("disc not resolved\n");
        if (params->cdio) {
            cdio_destroy (params->cdio);
        }
        cddb_disc_destroy (disc);
        free (params);
        return;
    }
    trace ("disc resolved\n");

    const char *disc_title = cddb_disc_get_title (disc);
    const char *artist = cddb_disc_get_artist (disc);
    const char *genre = cddb_disc_get_genre (disc);
    const unsigned int year = cddb_disc_get_year (disc);
    trace ("disc_title=%s, artist=%s, genre=%s, year=%d\n", disc_title, artist, genre, year);
    int track_count = cddb_disc_get_track_count(disc);
    char num_tracks[5];
    snprintf(num_tracks, sizeof(num_tracks), "%02d", track_count);

    for (int i = 0; items[i]; i++)
    {
        deadbeef->pl_add_meta (items[i], CDDB_IDS_TAG, disc_list);
        write_metadata(items[i], disc, num_tracks, i, artist, disc_title, genre, year);
    }
    cddb_disc_destroy (disc);
    cleanup_thread_params (params);
    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    if (plt)
    {
        deadbeef->plt_modified (plt);
        deadbeef->plt_unref (plt);
    }
    deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, 0, 0);
}

static void
read_track_cdtext (CdIo_t *cdio, int track_nr, DB_playItem_t *item)
{
#if CDIO_API_VERSION >= 6
    cdtext_t *cdtext = cdio_get_cdtext (cdio);
#else
    cdtext_t *cdtext = cdio_get_cdtext (cdio, 0);
#endif
    if (!cdtext)
    {
        trace ("No cdtext\n");
        return;
    }
    const char *artist = NULL;
    const char *album = NULL;
    int field_type;
    for (field_type = 0; field_type < MAX_CDTEXT_FIELDS; field_type++)
    {
#if CDIO_API_VERSION >= 6
        const char *text = cdtext_get_const (cdtext, field_type, track_nr);
#else
        const char *text = cdtext_get_const (field_type, cdtext);
#endif
        const char *field = NULL;
        if (text)
        {
            switch (field_type)
            {
#if CDIO_API_VERSION >= 6
                case CDTEXT_FIELD_TITLE: album = text; break;
                case CDTEXT_FIELD_PERFORMER: artist = text; break;
#else
                case CDTEXT_TITLE: album = text; break;
                case CDTEXT_PERFORMER: artist = text; break;
#endif
            }
        }
    }

    trace ("artist: %s; album: %s\n", artist, album);
    if (artist) {
        deadbeef->pl_replace_meta (item, "artist", artist);
    }
    if (album) {
        deadbeef->pl_replace_meta (item, "album", album);
    }

#if CDIO_API_VERSION >= 6
    cdtext = cdio_get_cdtext (cdio);
#else
    cdtext = cdio_get_cdtext (cdio, track_nr);
#endif
    if (!cdtext)
        return;

    for (field_type = 0; field_type < MAX_CDTEXT_FIELDS; field_type++)
    {
#if CDIO_API_VERSION >= 6
        const char *text = cdtext_get_const (cdtext, field_type, track_nr);
#else
        const char *text = cdtext_get_const (field_type, cdtext);
#endif
        const char *field = NULL;
        if (!text)
            continue;
        switch (field_type)
        {
#if CDIO_API_VERSION >= 6
            case CDTEXT_FIELD_TITLE:      field = "title";    break;
            case CDTEXT_FIELD_PERFORMER:  field = "artist";   break;
            case CDTEXT_FIELD_COMPOSER:   field = "composer"; break;
            case CDTEXT_FIELD_GENRE:      field = "genre";    break;
            case CDTEXT_FIELD_SONGWRITER: field = "songwriter";   break;
            case CDTEXT_FIELD_MESSAGE:    field = "comment";  break;
#else
            case CDTEXT_TITLE:      field = "title";    break;
            case CDTEXT_PERFORMER:  field = "artist";   break;
            case CDTEXT_COMPOSER:   field = "composer"; break;
            case CDTEXT_GENRE:      field = "genre";    break;
            case CDTEXT_SONGWRITER: field = "songwriter";   break;
            case CDTEXT_MESSAGE:    field = "comment";  break;
#endif
            default: field = NULL;
        }
        if (field && text)
        {
            trace ("%s: %s\n", field, text);
            deadbeef->pl_replace_meta (item, field, text);
        }
    }
}

static int
read_disc_cdtext (struct cddb_thread_params *params)
{
    DB_playItem_t **items = params->items;
#if CDIO_API_VERSION >= 6
    cdtext_t *cdtext = cdio_get_cdtext (params->cdio);
#else
    cdtext_t *cdtext = cdio_get_cdtext (params->cdio, 0);
#endif
    if (!cdtext)
        return 0;

    track_t first_track = cdio_get_first_track_num (params->cdio);
    track_t tracks = cdio_get_num_tracks (params->cdio);
    track_t i;
    for (i = 0; i < tracks; i++)
        read_track_cdtext (params->cdio, i + first_track, params->items[i]);

    return 1;
}

static DB_playItem_t *
cda_insert (ddb_playlist_t *plt, DB_playItem_t *after, const char *fname) {
    trace ("CDA insert: %s\n", fname);
    DB_playItem_t *res;

    const char* shortname = strrchr (fname, '/');
    if (shortname) {
        shortname++;
    }
    else {
        shortname = fname;
    }
    const char *ext = strrchr (shortname, '.') + 1;
    int is_image = ext && (0 == strcmp (ext, "nrg"));
    if (is_image && !deadbeef->conf_get_int ("cdda.enable_nrg", 0)) {
        return NULL;
    }

    cdio_close_tray (NULL, NULL);
    CdIo_t* cdio = NULL;
    if (0 == strcmp (ext, "cda")) {
        cdio = cdio_open (NULL, DRIVER_UNKNOWN);
    }
    else if (is_image) {
        cdio = cdio_open (fname, DRIVER_NRG);
    }
    if (!cdio) {
        trace ("not an audio disc/image, or file not found (%s)\n", fname);
        return NULL;
    }

    unsigned long discid = calc_discid(cdio);
    if (!discid)
    {
        trace ("cdda: no discid found\n");
        cdio_destroy(cdio);
        return NULL;
    }

    if (0 == strcasecmp (shortname, "all.cda") || is_image)
    {
        res = after;
        struct cddb_thread_params *p = malloc(sizeof(struct cddb_thread_params));
        if (!p)
        {
            cdio_destroy(cdio);
            return NULL;
        }

        const track_t first_track = cdio_get_first_track_num(cdio);
        const track_t tracks = cdio_get_num_tracks(cdio);
        p->items = malloc((tracks+1) * sizeof(*p->items));
        if (!p->items)
        {
            free(p);
            cdio_destroy(cdio);
            return NULL;
        }

        for (track_t i = 0; i < tracks; i++)
        {
            trace ("inserting track %d\n", i);
            res = insert_single_track (cdio, plt, res, is_image ? fname : NULL, i+first_track, discid);
            p->items[i] = res;
        }
        p->items[tracks] = NULL;

        p->cdio = cdio;
        const int got_cdtext = read_disc_cdtext(p);
        const int prefer_cdtext = deadbeef->conf_get_int ("cdda.prefer_cdtext", DEFAULT_PREFER_CDTEXT);
        const int enable_cddb = deadbeef->conf_get_int ("cdda.freedb.enable", DEFAULT_USE_CDDB);
        if ((!got_cdtext || !prefer_cdtext) && enable_cddb)
        {
            trace ("cdda: querying freedb...\n");
            intptr_t tid = deadbeef->thread_start (cddb_thread, p); //will destroy cdio
            deadbeef->thread_detach (tid);
        }
        else
        {
            cleanup_thread_params (p);
        }
    }
    else
    {
        const int track_nr = atoi (shortname);
        res = insert_single_track (cdio, plt, after, NULL, track_nr, discid);
        if (res)
        {
            read_track_cdtext (cdio, track_nr, res);
            deadbeef->pl_item_unref (res);
        }
        cdio_destroy (cdio);
    }
    return res;
}

static int
cda_action_disc_n (const size_t disc_num)
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if (!plt)
        return -1;

    /* Find the first selected playitem */
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    while (it && !deadbeef->pl_is_selected(it))
    {
        deadbeef->pl_item_unref(it);
        it = deadbeef->pl_get_next(it, PL_MAIN);
    }

    /* Find the category/discid for the chosen disc */
    deadbeef->pl_lock();
    const char *disc_list = deadbeef->pl_find_meta(it, CDDB_IDS_TAG);
    char *p = (char *)disc_list;
    size_t i = 0;
    while (p && i++ < disc_num)
    {
        p = strchr(++p, ',');
    }
    char category[CDDB_CATEGORY_SIZE];
    char discid_string[CDDB_DISCID_SIZE];
    sscanf(p+1, "%[^/]/%8s", category, discid_string);
    deadbeef->pl_unlock();

    /* Read the cddb data for this disc */
    cddb_disc_t *disc = cddb_disc_new();
    cddb_disc_set_category_str(disc, category);
    const unsigned long discid = strtoul(discid_string, NULL, 16);
    cddb_disc_set_discid(disc, discid);
    cddb_conn_t *conn = new_cddb_connection();
    if (conn)
    {
        cddb_read(conn, disc);
        cddb_destroy(conn);
    }
    const char *disc_title = cddb_disc_get_title(disc);
    const char *artist = cddb_disc_get_artist(disc);
    const char *genre = cddb_disc_get_genre(disc);
    const unsigned int year = cddb_disc_get_year(disc);
    trace("disc_title=%s, disk_artist=%s\n", disc_title, artist);
    int track_count = cddb_disc_get_track_count(disc);
    char num_tracks[5];
    snprintf(num_tracks, sizeof(num_tracks), "%02d", track_count);

    /* Apply the data to each selected playitem */
    do
    {
        if (deadbeef->pl_is_selected(it))
        {
            const int track_index = deadbeef->pl_find_meta_int(it, "track", 0) - 1;
            write_metadata(it, disc, num_tracks, track_index, artist, disc_title, genre, year);
        }
        deadbeef->pl_item_unref(it);
        it = deadbeef->pl_get_next(it, PL_MAIN);
    } while (it);

    deadbeef->plt_modified(plt);
    deadbeef->plt_unref(plt);
    cddb_disc_destroy(disc);
    deadbeef->sendmessage(DB_EV_PLAYLISTCHANGED, 0, 0, 0);
}

static int cda_action_disc0(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(0);}
static int cda_action_disc1(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(1);}
static int cda_action_disc2(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(2);}
static int cda_action_disc3(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(3);}
static int cda_action_disc4(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(4);}
static int cda_action_disc5(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(5);}
static int cda_action_disc6(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(6);}
static int cda_action_disc7(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(7);}
static int cda_action_disc8(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(8);}
static int cda_action_disc9(DB_plugin_action_t *act, int ctx) {return cda_action_disc_n(9);}

static DB_plugin_action_t disc_actions[MAX_CDDB_DISCS];
static char disc_action_titles[MAX_CDDB_DISCS][MAX_CDDB_MENU];

static int
cda_action_add_cd (DB_plugin_action_t *act, int ctx)
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    if (plt) {
        deadbeef->plt_add_files_begin (plt, 0);
        deadbeef->plt_add_file2 (0, plt, "all.cda", NULL, NULL);
        deadbeef->plt_add_files_end (plt, 0);
        deadbeef->plt_modified (plt);
        deadbeef->plt_unref (plt);
    }
    deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, 0, 0);
    return 0;
}

static DB_plugin_action_t add_cd_action = {
    .name = "cd_add",
    .title = "File/Add Audio CD",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = cda_action_add_cd,
    .next = NULL
};

static DB_plugin_action_t *
cda_get_actions (DB_playItem_t *it)
{
    /* Main menu, File->Add Audio CD */
    if (!it) {
        return &add_cd_action;
    }

    /* Quick sanity check that the current playitem is a CD track with a CDDB IDs tag */
    char disc_list[(CDDB_CATEGORY_SIZE + CDDB_DISCID_SIZE) * MAX_CDDB_DISCS + 1] = "";
    deadbeef->pl_get_meta(it, CDDB_IDS_TAG, disc_list, sizeof(disc_list));
    if (!*disc_list)
    {
        return NULL;
    }

    /* Make sure all selected playitems are from the same CD disc */
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if (!plt)
    {
        return NULL;
    }
    DB_playItem_t *test_it = deadbeef->plt_get_first(plt, PL_MAIN);
    while (test_it)
    {
        if (deadbeef->pl_is_selected(test_it))
        {
            deadbeef->pl_lock();
            const char *it_disc_list = deadbeef->pl_find_meta(test_it, CDDB_IDS_TAG);
            if (!it_disc_list || strcmp(disc_list, it_disc_list))
            {
                deadbeef->pl_item_unref(test_it);
                deadbeef->plt_unref(plt);
                deadbeef->pl_unlock();
                return NULL;
            }
            deadbeef->pl_unlock();
        }

        deadbeef->pl_item_unref(test_it);
        test_it = deadbeef->pl_get_next(test_it, PL_MAIN);
    }
    deadbeef->plt_unref(plt);

    /* Make sure all the static menu items are initialised */
    if (!disc_actions[0].name)
    {
        disc_actions[0].name = "disc_action0";
        disc_actions[0].callback2 = cda_action_disc0;
        disc_actions[1].name = "disc_action1";
        disc_actions[1].callback2 = cda_action_disc1;
        disc_actions[2].name = "disc_action2";
        disc_actions[2].callback2 = cda_action_disc2;
        disc_actions[3].name = "disc_action3";
        disc_actions[3].callback2 = cda_action_disc3;
        disc_actions[4].name = "disc_action4";
        disc_actions[4].callback2 = cda_action_disc4;
        disc_actions[5].name = "disc_action5";
        disc_actions[5].callback2 = cda_action_disc5;
        disc_actions[6].name = "disc_action6";
        disc_actions[6].callback2 = cda_action_disc6;
        disc_actions[7].name = "disc_action7";
        disc_actions[7].callback2 = cda_action_disc7;
        disc_actions[8].name = "disc_action8";
        disc_actions[8].callback2 = cda_action_disc8;
        disc_actions[9].name = "disc_action9";
        disc_actions[9].callback2 = cda_action_disc9;
        for (size_t i = 0; i < MAX_CDDB_DISCS; i++)
        {
            disc_actions[i].title = disc_action_titles[i];
        }
    }

    /* Only query against the cache, everything has been read before and we need to be fast */
    cddb_conn_t *conn = new_cddb_connection();
    if (!conn)
    {
        return NULL;
    }
    cddb_cache_only(conn);
    cddb_disc_t *disc = cddb_disc_new();
    if (!disc)
    {
        cddb_destroy(conn);
        return NULL;
    }

    /* Build a menu item for each possible cddb disc for this CD */
    size_t i = 0;
    char category[CDDB_CATEGORY_SIZE];
    char *p = disc_list;
    do {
        char discid_string[10];
        sscanf(++p, "%[^/]/%8s", category, discid_string);
        cddb_disc_set_category_str(disc, category);
        const unsigned long discid = strtoul(discid_string, NULL, 16);
        cddb_disc_set_discid(disc, discid);
        if (cddb_read(conn, disc))
        {
            const char *title = cddb_disc_get_title(disc);
            const unsigned int year_int = cddb_disc_get_year(disc);
            char year[8] = "";
            if (year_int && year_int <= 9999)
            {
                sprintf(year, "[%u] ", year_int);
            }
            snprintf(disc_action_titles[i], MAX_CDDB_MENU, "Load CDDB metadata/%s%s", year, title);
            disc_actions[i].flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU;
            disc_actions[i].next = &disc_actions[i+1];
            i++;
        }
        p = strchr(p, ',');
    } while (p);
    disc_actions[i-1].next = NULL;

    cddb_disc_destroy(disc);
    cddb_destroy(conn);

    return disc_actions;
}

static const char *exts[] = { "cda", "nrg", NULL };
static const char settings_dlg[] =
    "property \"Use CDDB/FreeDB\" checkbox cdda.freedb.enable 1;\n"
    "property \"Prefer CD-Text over CDDB\" checkbox cdda.prefer_cdtext 1;\n"
    "property \"CDDB url (e.g. 'freedb.org')\" entry cdda.freedb.host freedb.org;\n"
    "property \"CDDB port number (e.g. '888')\" entry cdda.freedb.port 888;\n"
    "property \"Prefer CDDB protocol over HTTP\" checkbox cdda.protocol 1;\n"
    "property \"Enable NRG image support\" checkbox cdda.enable_nrg 0;"
#if USE_PARANOIA
    "property \"Use cdparanoia error correction when ripping\" checkbox cdda.paranoia 0;"
#endif
;

// define plugin interface
static DB_decoder_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 0,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_DECODER,
    .plugin.id = "cda",
    .plugin.name = "Audio CD player",
    .plugin.descr = "Audio CD plugin using libcdio and libcddb",
    .plugin.copyright =
        "Copyright (C) 2009-2013 Alexey Yakovenko <waker@users.sourceforge.net>\n"
        "Copyright (C) 2009-2011 Viktor Semykin <thesame.ml@gmail.com>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website = "http://deadbeef.sf.net",
    .plugin.configdialog = settings_dlg,
    .plugin.get_actions = cda_get_actions,
    .open = cda_open,
    .init = cda_init,
    .free = cda_free,
    .read = cda_read,
    .seek = cda_seek,
    .seek_sample = cda_seek_sample,
    .insert = cda_insert,
    .exts = exts,
};

DB_plugin_t *
cdda_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

