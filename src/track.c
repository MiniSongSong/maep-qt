/*
 * This file is part of Maep.
 *
 * Maep is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Maep is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Maep.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE /* glibc2 needs this */
#define __USE_XOPEN
#include <time.h>

#include "config.h"
#include "track.h"
#include "osm-gps-map.h"
#include "converter.h"
#include "misc.h"
#include "menu.h"
#include "gps.h"

#include <math.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>
#include <strings.h>

#define DATE_FORMAT "%FT%T"
#define TRACK_CAPTURE_ENABLED "track_capture_enabled"

#ifndef LIBXML_TREE_ENABLED
#error "Tree not enabled in libxml"
#endif

#ifdef USE_MAEMO
#define GTK_FM_OK  GTK_RESPONSE_OK
#else
#define GTK_FM_OK  GTK_RESPONSE_ACCEPT
#endif

static void filemgr_setup(GtkWidget *dialog, gboolean save) {
  char *track_path = gconf_get_string("track_path");

  if(track_path) {
    if(!g_file_test(track_path, G_FILE_TEST_IS_REGULAR)) {
      char *last_sep = strrchr(track_path, '/');
      if(last_sep) {
	*last_sep = 0;  // seperate path from file 
	
	/* the user just created a new document */
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), 
					    track_path);
	if(save)
	  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), 
					    last_sep+1);
      }
    } else 
      gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), track_path);
  }
}

static gboolean track_get_prop_pos(xmlNode *node, coord_t *pos) {
  char *str_lat = (char*)xmlGetProp(node, BAD_CAST "lat");
  char *str_lon = (char*)xmlGetProp(node, BAD_CAST "lon");

  if(!str_lon || !str_lat) {
    if(!str_lon) xmlFree(str_lon);
    if(!str_lat) xmlFree(str_lat);
    return FALSE;
  }

  pos->rlat = deg2rad(g_ascii_strtod(str_lat, NULL));
  pos->rlon = deg2rad(g_ascii_strtod(str_lon, NULL));

  xmlFree(str_lon);
  xmlFree(str_lat);

  return TRUE;
}

static track_point_t *track_parse_trkpt(xmlDocPtr doc, xmlNode *a_node) {
  track_point_t *point = g_new0(track_point_t, 1);

  /* parse position */
  if(!track_get_prop_pos(a_node, &point->coord)) {
    g_free(point);
    return NULL;
  }

  /* scan for children */
  xmlNode *cur_node = NULL;
  for (cur_node = a_node->children; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {

      /* elevation (altitude) */
      if(strcasecmp((char*)cur_node->name, "ele") == 0) {
	char *str = (char*)xmlNodeGetContent(cur_node);
	point->altitude = g_ascii_strtod(str, NULL);
 	xmlFree(str);
      }

      /* time */
      if(strcasecmp((char*)cur_node->name, "time") == 0) {
	struct tm time;
	char *str = (char*)xmlNodeGetContent(cur_node);
	char *ptr = strptime(str, DATE_FORMAT, &time);
	if(ptr) point->time = mktime(&time);
 	xmlFree(str);
      }
    }
  }

  return point;
}

static void 
track_parse_trkseg(track_t *track, xmlDocPtr doc, xmlNode *a_node) {
  xmlNode *cur_node = NULL;
  track_point_t **point = NULL;
  track_seg_t **seg = &(track->track_seg);

  /* search end of track_seg list */
  while(*seg) seg = &((*seg)->next);

  for (cur_node = a_node->children; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {
      if(strcasecmp((char*)cur_node->name, "trkpt") == 0) {
	track_point_t *cpnt = track_parse_trkpt(doc, cur_node);
	if(cpnt) {
	  if(!point) {
	    /* start a new segment */
	    *seg = g_new0(track_seg_t, 1);
	    point = &((*seg)->track_point);
	  }
	  /* attach point to chain */
	  *point = cpnt;
	  point = &((*point)->next);
	} else {
	  /* end segment if point could not be parsed and start a new one */
	  /* close segment if there is one */
	  if(point) {
	    printf("ending track segment leaving bounds\n");
	    seg = &((*seg)->next);
	    point = NULL;
	  }
	}
      } else
	printf("found unhandled gpx/trk/trkseg/%s\n", cur_node->name);
      
    }
  }
}

static track_t *track_parse_trk(xmlDocPtr doc, xmlNode *a_node) {
  track_t *track = g_new0(track_t, 1);
  xmlNode *cur_node = NULL;

  for (cur_node = a_node->children; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {
      if(strcasecmp((char*)cur_node->name, "trkseg") == 0) {
	track_parse_trkseg(track, doc, cur_node);
      } else
	printf("found unhandled gpx/trk/%s\n", cur_node->name);
      
    }
  }
  return track;
}

static track_t *track_parse_gpx(xmlDocPtr doc, xmlNode *a_node) {
  track_t *track = NULL;
  xmlNode *cur_node = NULL;

  for (cur_node = a_node->children; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {
      if(strcasecmp((char*)cur_node->name, "trk") == 0) {
	if(!track) 
	  track = track_parse_trk(doc, cur_node);
	else
	  printf("ignoring additional track\n");
      } else
	printf("found unhandled gpx/%s\n", cur_node->name);      
    }
  }
  return track;
}

/* parse root element and search for "track" */
static track_t *track_parse_root(xmlDocPtr doc, xmlNode *a_node) {
  track_t *track = NULL;
  xmlNode *cur_node = NULL;

  for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {
      /* parse track file ... */
      if(strcasecmp((char*)cur_node->name, "gpx") == 0) 
      	track = track_parse_gpx(doc, cur_node);
      else 
	printf("found unhandled %s\n", cur_node->name);
    }
  }
  return track;
}

static track_t *track_parse_doc(xmlDocPtr doc) {
  track_t *track;

  /* Get the root element node */
  xmlNode *root_element = xmlDocGetRootElement(doc);

  track = track_parse_root(doc, root_element);  

  /*free the document */
  xmlFreeDoc(doc);

  /*
   * Free the global variables that may
   * have been allocated by the parser.
   */
  xmlCleanupParser();

  return track;
}

static track_t *track_read(char *filename) {
  xmlDoc *doc = NULL;

  LIBXML_TEST_VERSION;
  
  /* parse the file and get the DOM */
  if((doc = xmlReadFile(filename, NULL, 0)) == NULL) {
    xmlErrorPtr	errP = xmlGetLastError();
    printf("While parsing \"%s\":\n\n%s\n", filename, errP->message);
    return NULL;
  }

  track_t *track = track_parse_doc(doc); 

  if(!track || !track->track_seg) {
    printf("track was empty/invalid track\n");
    return NULL;
  }

  track->dirty = TRUE;
  
  return track;
}

void track_draw(GtkWidget *map, track_t *track) {
  /* erase any previous track */
  track_clear(map);

  if(!track) return;

  track_seg_t *seg = track->track_seg;
  while(seg) {
    GSList *points = NULL;
    track_point_t *point = seg->track_point;
    
    while(point) {
      /* we need to create a copy of the coordinate since */
      /* the map will free them */
      coord_t *new_point = g_memdup(&point->coord, sizeof(coord_t));
      points = g_slist_append(points, new_point);
      point = point->next;
    }
    seg = seg->next;
    
    osm_gps_map_add_track(OSM_GPS_MAP(map), points);
  }

  /* save track reference in map */
  g_object_set_data(G_OBJECT(map), "track", track);

  GtkWidget *toplevel = gtk_widget_get_toplevel(map);
  menu_enable(toplevel, "Track/Clear", TRUE);
  menu_enable(toplevel, "Track/Export", TRUE);
}

/* this imports a track and adds it to the set of existing tracks */
void track_import(GtkWidget *map) {
  GtkWidget *toplevel = gtk_widget_get_toplevel(map);
  
  /* open a file selector */
  GtkWidget *dialog;

  track_t *track = NULL;
  
#ifdef USE_HILDON
  dialog = hildon_file_chooser_dialog_new(GTK_WINDOW(toplevel), 
					  GTK_FILE_CHOOSER_ACTION_OPEN);
#else
  dialog = gtk_file_chooser_dialog_new (_("Import track file"),
			GTK_WINDOW(toplevel),
			GTK_FILE_CHOOSER_ACTION_OPEN,
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
			NULL);
#endif

  filemgr_setup(dialog, FALSE);
  gtk_widget_show_all (GTK_WIDGET(dialog));
  if (gtk_dialog_run (GTK_DIALOG(dialog)) == GTK_FM_OK) {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

    /* load a track */
    track = track_read(filename);
    if(track) 
      gconf_set_string("track_path", filename);

    g_free (filename);

    track_draw(map, track);
  }

  gtk_widget_destroy (dialog);
}

/* --------------------------------------------------------------- */

void track_point_free(track_point_t *point) {
  g_free(point);
}

void track_seg_free(track_seg_t *seg) {
  track_point_t *point = seg->track_point;
  while(point) {
    track_point_t *next = point->next;
    track_point_free(point);
    point = next;
  }

  g_free(seg);
}

void track_clear(GtkWidget *map) {
  track_t *track = g_object_get_data(G_OBJECT(map), "track");
  if (!track) return;

  g_object_set_data(G_OBJECT(map), "track", NULL);

  GtkWidget *toplevel = gtk_widget_get_toplevel(map);
  menu_enable(toplevel, "Track/Clear", FALSE);
  menu_enable(toplevel, "Track/Export", FALSE);

  osm_gps_map_clear_tracks(OSM_GPS_MAP(map));

  track_seg_t *seg = track->track_seg;
  while(seg) {
    track_seg_t *next = seg->next;
    track_seg_free(seg);
    seg = next;
  }
  g_free(track);
}

/* this callback is called from the gps layer as long as */
/* captureing is enabled */
static void gps_callback(int status, struct gps_fix_t *fix, void *data) {
  static coord_t last = { NAN, NAN };

  OsmGpsMap *map = OSM_GPS_MAP(data);

  /* save gps position in track */
  
  if(status) {
    if(!isnan(last.rlat) && !isnan(last.rlon)) {

      GSList *points = NULL;
      coord_t *new_point = g_memdup(&last, sizeof(coord_t));
      points = g_slist_append(points, new_point);
      new_point = g_new0(coord_t, 1);
      new_point->rlat = deg2rad(fix->latitude);
      new_point->rlon = deg2rad(fix->longitude);
      points = g_slist_append(points, new_point);
    
      osm_gps_map_add_track(OSM_GPS_MAP(map), points);
    }

    last.rlat = deg2rad(fix->latitude);
    last.rlon = deg2rad(fix->longitude);

  } else {
    printf("interrupting track\n");
    last.rlat = last.rlon = NAN;
  }
}

void track_capture_enable(GtkWidget *map, gboolean enable) {
  printf("%sabling track capture\n", enable?"en":"dis");

  /* verify that tracking isn't already in the requested state */
  gboolean cur_state = 
    (gboolean)g_object_get_data(G_OBJECT(map), TRACK_CAPTURE_ENABLED);

  g_assert(cur_state != enable);

  /* save new tracking state */
  g_object_set_data(G_OBJECT(map), TRACK_CAPTURE_ENABLED, (gpointer)enable);

  gps_state_t *gps_state = g_object_get_data(G_OBJECT(map), "gps_state");

  if(enable) 
    gps_register_callback(gps_state, gps_callback, map);
  else {
    gps_unregister_callback(gps_state, gps_callback);
    gps_callback(0, NULL, map);
  }
}

/* ----------------------  saving track --------------------------- */

void track_save_points(track_point_t *point, xmlNodePtr node) {
  while(point) {
    char str[G_ASCII_DTOSTR_BUF_SIZE];

    xmlNodePtr node_point = xmlNewChild(node, NULL, BAD_CAST "trkpt", NULL);

    g_ascii_formatd(str, sizeof(str), "%.07f", rad2deg(point->coord.rlat));
    xmlNewProp(node_point, BAD_CAST "lat", BAD_CAST str);
    
    g_ascii_formatd(str, sizeof(str), "%.07f", rad2deg(point->coord.rlon));
    xmlNewProp(node_point, BAD_CAST "lon", BAD_CAST str);

    if(!isnan(point->altitude)) {
      g_ascii_formatd(str, sizeof(str), "%.02f", point->altitude);
      xmlNewTextChild(node_point, NULL, BAD_CAST "ele", BAD_CAST str);
    }

    if(point->time) {
      strftime(str, sizeof(str), DATE_FORMAT, localtime(&point->time));
      xmlNewTextChild(node_point, NULL, BAD_CAST "time", BAD_CAST str);
    }

    point = point->next;
  }
}

void track_save_segs(track_seg_t *seg, xmlNodePtr node) {
  while(seg) {
    xmlNodePtr node_seg = xmlNewChild(node, NULL, BAD_CAST "trkseg", NULL);
    track_save_points(seg->track_point, node_seg);
    seg = seg->next;
  }
}

void track_write(char *name, track_t *track) {
  LIBXML_TEST_VERSION;
 
  xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
  xmlNodePtr root_node = xmlNewNode(NULL, BAD_CAST "gpx");
  xmlNewProp(root_node, BAD_CAST "creator", BAD_CAST PACKAGE " v" VERSION);
  xmlNewProp(root_node, BAD_CAST "xmlns", BAD_CAST 
	     "http://www.topografix.com/GPX/1/0");

  xmlNodePtr trk_node = xmlNewChild(root_node, NULL, BAD_CAST "trk", NULL);
  xmlDocSetRootElement(doc, root_node);
  
  track_save_segs(track->track_seg, trk_node);

  xmlSaveFormatFileEnc(name, doc, "UTF-8", 1);
  xmlFreeDoc(doc);
  xmlCleanupParser();

  track->dirty = FALSE;
}

void track_export(GtkWidget *map) {
  GtkWidget *toplevel = gtk_widget_get_toplevel(map);
  track_t *track = g_object_get_data(G_OBJECT(map), "track");

  /* the menu should be disabled when no track is present */
  g_assert(track);

  /* open a file selector */
  GtkWidget *dialog;

#ifdef USE_HILDON
  dialog = hildon_file_chooser_dialog_new(GTK_WINDOW(toplevel), 
					  GTK_FILE_CHOOSER_ACTION_SAVE);
#else
  dialog = gtk_file_chooser_dialog_new(_("Export track file"),
				       GTK_WINDOW(toplevel),
				       GTK_FILE_CHOOSER_ACTION_SAVE,
				       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				       GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
				       NULL);
#endif

  filemgr_setup(dialog, TRUE);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_FM_OK) {
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if(filename) {
      printf("export to %s\n", filename);

      if(!g_file_test(filename, G_FILE_TEST_EXISTS) ||
	 yes_no_f(dialog, _("Overwrite existing file?"), 
		  _("The file already exists. "
		    "Do you really want to replace it?"))) {

	gconf_set_string("track_path", filename);

	track_write(filename, track);
      }
    }
  }
  
  gtk_widget_destroy (dialog);
}

#ifdef USE_MAEMO
#ifdef MAEMO5
#define TRACK_PATH  "/home/user/." APP
#else
#define TRACK_PATH  "/media/mmc2/" APP
#endif
#else
#define TRACK_PATH  "~/." APP
#endif

static char *build_path(void) {
  const char track_path[] = TRACK_PATH;

  if(track_path[0] == '~') {
    int skip = 1;
    char *p = getenv("HOME");
    if(!p) return NULL;

    while(track_path[strlen(track_path)-skip] == '/')
      skip++;

    return g_strdup_printf("%s/%s/track.trk", p, track_path+skip);
  }

  return g_strdup_printf("%s/track.trk", track_path);
}

void track_restore(GtkWidget *map) {
  char *path = build_path();

  if(g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    track_t *track = track_read(path);

    if(track) 
      track_draw(map, track);

    /* the track comes fresh from the disk */
    track->dirty = FALSE;
  } else {
    GtkWidget *toplevel = gtk_widget_get_toplevel(map);
    menu_enable(toplevel, "Track/Clear", FALSE);
    menu_enable(toplevel, "Track/Export", FALSE);
  }

  g_free(path);

  /* we may also have to restore track capture */
  if(gconf_get_bool(TRACK_CAPTURE_ENABLED, FALSE)) {
    GtkWidget *toplevel = gtk_widget_get_toplevel(map);
    menu_check_set_active(toplevel, "Track/Capture", TRUE);
  }
}

void track_save(GtkWidget *map) {
  /* save state of capture engine */
  gconf_set_bool(TRACK_CAPTURE_ENABLED, 
	 (gboolean)g_object_get_data(G_OBJECT(map), TRACK_CAPTURE_ENABLED));

  char *path = build_path();
  track_t *track = g_object_get_data(G_OBJECT(map), "track");
  if(!track) {
    remove(path);
    g_free(path);
    return;
  }

  if(!track->dirty) {
    g_free(path);
    return;
  }

  /* make sure directory exists */
  char *last_sep = strrchr(path, '/');
  g_assert(last_sep);
  *last_sep = 0;
  g_mkdir_with_parents(path, 0700);
  *last_sep = '/';

  track_write(path, track);
  
  g_free(path);
}
