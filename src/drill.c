/*
 * gEDA - GNU Electronic Design Automation
 * drill.c
 * Copyright (C) 2000-2006 Andreas Andersson
 *
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * 21 Feb 2007 patch for metric drill files:
 * 1) METRIC/INCH commands (partly) parsed to define units of the header
 * 2) units of the header and the program body are independent
 * 3) ICI command parsed in the header
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <glib.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <math.h>  /* pow() */
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "drill.h"
#include "gerb_error.h"
#include "tooltable.h"
#include "drill_stats.h"

#include "common.h"

/* DEBUG printing.  #define DEBUG 1 in config.h to use this fcn. */
#define dprintf if(DEBUG) printf

#define NOT_IMPL(fd, s) do { \
                             GERB_MESSAGE("Not Implemented:%s\n", s); \
                           } while(0)

#define MAXL 200


#undef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#undef min
#define min(a,b) ((a) < (b) ? (a) : (b))

enum drill_file_section_t {DRILL_NONE, DRILL_HEADER, DRILL_DATA};
enum drill_coordinate_mode_t {DRILL_MODE_ABSOLUTE, DRILL_MODE_INCREMENTAL};

enum drill_m_code_t {DRILL_M_UNKNOWN, DRILL_M_NOT_IMPLEMENTED,
		     DRILL_M_END, DRILL_M_ENDREWIND,
		     DRILL_M_MESSAGE, DRILL_M_LONGMESSAGE,
		     DRILL_M_HEADER, DRILL_M_ENDHEADER,
		     DRILL_M_METRIC, DRILL_M_IMPERIAL,
		     DRILL_M_BEGINPATTERN, DRILL_M_ENDPATTERN,
		     DRILL_M_CANNEDTEXT, DRILL_M_TIPCHECK,
		     DRILL_M_METRICHEADER, DRILL_M_IMPERIALHEADER};


enum drill_g_code_t {DRILL_G_ABSOLUTE, DRILL_G_INCREMENTAL,
		     DRILL_G_ZEROSET, DRILL_G_UNKNOWN,
		     DRILL_G_ROUT, DRILL_G_DRILL,
		     DRILL_G_LINEARMOVE, DRILL_G_CWMOVE, DRILL_G_CCWMOVE};

enum number_fmt_t {FMT_00_0000 /* INCH */,
		   FMT_000_000 /* METRIC 6-digit, 1 um */,
		   FMT_000_00  /* METRIC 5-digit, 10 um */,
		   FMT_0000_00 /* METRIC 6-digit, 10 um */,
		   FMT_USER    /* User defined format */};

typedef struct drill_state {
    double curr_x;
    double curr_y;
    int current_tool;
    int curr_section;
    int coordinate_mode;
    double origin_x;
    double origin_y;
    enum unit_t unit;
    /* number_format is used throughout the file itself.

       header_number_format is used to parse the tool definition C
       codes within the header.  It is fixed to FMT_00_0000 for INCH
       measures, and FMT_000_000 (1 um resolution) for metric
       measures. */
    enum number_fmt_t number_format, header_number_format;
    /* Used as a backup when temporarily switching to INCH. */
    enum number_fmt_t backup_number_format;

    /* 0 means we don't try to autodetect any of the other values */
    int autod;
    
    /* in FMT_USER this specifies the number of digits after the decimal 
     * place in the file
     */
    int decimals;

} drill_state_t;

/* Local function prototypes */
static int drill_parse_G_code(gerb_file_t *fd, gerb_image_t *image);
static int drill_parse_M_code(gerb_file_t *fd, drill_state_t *state, 
			      gerb_image_t *image);
static int drill_parse_T_code(gerb_file_t *fd, drill_state_t *state, 
			      gerb_image_t *image);
static void drill_parse_coordinate(gerb_file_t *fd, char firstchar, 
				   gerb_image_t *image, drill_state_t *state);
static drill_state_t *new_state(drill_state_t *state);
static double read_double(gerb_file_t *fd, enum number_fmt_t fmt, 
			  enum omit_zeros_t omit_zeros, int decimals);
static void eat_line(gerb_file_t *fd);
static char *get_line(gerb_file_t *fd);

/* -------------------------------------------------------------- */
/* This is the list of specific attributes a drill file may have from
 * the point of view of parsing it.
 */

static const char *supression_list[] = {
    "None",
#define SUP_NONE 0
    "Leading",
#define SUP_LEAD 1
    "Trailing",
#define SUP_TRAIL 2
    0
};

static const char *units_list[] = {
    "inch",
#define UNITS_INCH 0
    "mil (1/1000 inch)",
#define UNITS_MIL 1
    "mm",
#define UNITS_MM 2
    0
};

static HID_Attribute drill_attribute_list[] = {
    /* This should be first */
  {"auto", "Autodetect file format",
   HID_Boolean, 0, 0, {1, 0, 0}, 0, 0},
#define HA_auto 0

  {"zero_supression", "Zero supression",
   HID_Enum, 0, 0, {0, 0, 0}, supression_list, 0},
#define HA_supression 1

  {"units", "Units",
   HID_Enum, 0, 0, {0, 0, 0}, units_list, 0},
#define HA_xy_units 2

#if 0
  {"tool_units", "Tool size units",
   HID_Enum, 0, 0, {0, 0, 0}, units_list, 0},
#define HA_tool_units 3
#endif

  {"digits", "Number of digits",
   HID_Integer, 0, 20, {5, 0, 0}, 0, 0},
#define HA_digits 3
};


/* -------------------------------------------------------------- */
gerb_image_t *
parse_drillfile(gerb_file_t *fd, HID_Attribute *attr_list, int n_attr, int reload)
{
    drill_state_t *state = NULL;
    gerb_image_t *image = NULL;
    gerb_net_t *curr_net = NULL;
    int read;
    drill_stats_t *stats;
    int i;
    char *tmps;

    /* Create new image for this layer */
    dprintf("In parse_drillfile, about to create image for this layer\n");
    image = new_gerb_image(image, "Excellon Drill File");
    if (image == NULL)
	GERB_FATAL_ERROR("malloc image failed\n");

    if (reload && attr_list != NULL)
	{
	    image->info->attr_list = attr_list;
	    image->info->n_attr = n_attr;
	}
    else
	{
	    /* Copy in the default attribute list for drill files.  We make a
	     * copy here because we will allow per-layer editing of the
	     * attributes.
	     */
	    image->info->n_attr = sizeof (drill_attribute_list) / sizeof (drill_attribute_list[0]);
	    image->info->attr_list = (HID_Attribute *) malloc (sizeof (drill_attribute_list));
	    if (image->info->attr_list == NULL)
		{
		    fprintf (stderr, "%s():  malloc failed\n", __FUNCTION__);
		    exit (1);
		}
	    dprintf ("%s(): New attribute list is %p\n", __FUNCTION__, image->info->attr_list);

	    for (i = 0 ; i < image->info->n_attr ; i++)
		{
		    image->info->attr_list[i] = drill_attribute_list[i];
		}

	    /* now merge any project attributes */
	    attribute_merge (image->info->attr_list, image->info->n_attr,
			     attr_list, n_attr);
	}

	    
    curr_net = image->netlist;
    curr_net->layer = image->layers;
    curr_net->state = image->states;
    image->layertype = DRILL;
    stats = drill_stats_new();
    if (stats == NULL)
	GERB_FATAL_ERROR("malloc stats failed\n");
    image->drill_stats = stats;

    /* Create local state variable to track photoplotter state */
    state = new_state(state);
    if (state == NULL)
	GERB_FATAL_ERROR("malloc state failed\n");

    image->format = (gerb_format_t *)g_malloc(sizeof(gerb_format_t));
    if (image->format == NULL)
	GERB_FATAL_ERROR("malloc format failed\n");
    memset((void *)image->format, 0, sizeof(gerb_format_t));
    image->format->omit_zeros = ZEROS_UNSPECIFIED;


    if (!image->info->attr_list[HA_auto].default_val.int_value) 
	{
	    state->autod = 0;
	    state->number_format = FMT_USER;
	    state->decimals = image->info->attr_list[HA_digits].default_val.int_value;
	    if (image->info->attr_list[HA_xy_units].default_val.int_value == UNITS_MM)
		state->unit = MM;
	    switch (image->info->attr_list[HA_supression].default_val.int_value)
		{
		case SUP_LEAD:
		    image->format->omit_zeros = LEADING;
		    break;

		case SUP_TRAIL:
		    image->format->omit_zeros = TRAILING;
		    break;

		default:
		    image->format->omit_zeros = EXPLICIT;
		    break;
		}
	}

    dprintf("%s():  Starting parsing of drill file\n", __FUNCTION__);
    while ((read = gerb_fgetc(fd)) != EOF) {

	switch ((char) read) {
	case ';' :
	    /* Comment found. Eat rest of line */
	    eat_line(fd);
	    break;
	case 'D' :
	    gerb_ungetc (fd);
	    tmps = get_line (fd);
	    if (strcmp (tmps, "DETECT,ON") == 0 ||
		strcmp (tmps, "DETECT,OFF") == 0) {
		char *tmps2;
		char *tmps3;
		if (strcmp (tmps, "DETECT,ON") == 0)
		    tmps3 = "ON";
		else
		    tmps3 = "OFF";

		/* broken tool detect on/off.  Silently ignored. */
		if (stats->detect) {
		    tmps2 = g_strdup_printf ("%s\n%s", stats->detect, tmps3);
		    g_free (stats->detect);
		} else {
		    tmps2 = g_strdup_printf ("%s", tmps3);
		}
		stats->detect = tmps2;
	    } else {
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Undefined header line = '%s'\n",tmps),
				      NOTE);
	    }
	    g_free (tmps);
	    break;
	case 'F' :
	    gerb_ungetc (fd);
	    tmps = get_line (fd);
	    /* Silently ignore FMAT,2.  Not sure what others are allowed */
	    if (strcmp (tmps, "FMAT,2") != 0) {
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Undefined header line = '%s'\n",tmps),
				      NOTE);
	    }
	    g_free (tmps);
	    break;

	case 'G':
	    /* Most G codes aren't used, for now */
	    switch(drill_parse_G_code(fd, image)) {
	    case DRILL_G_ROUT :
		drill_stats_add_error(stats->error_list,
				      -1,
				      "Rout mode data is not supported\n",
				      GRB_ERROR);
		break;
	    case DRILL_G_DRILL :
		break;
	    case DRILL_G_ABSOLUTE :
		state->coordinate_mode = DRILL_MODE_ABSOLUTE;
		break;
	    case DRILL_G_INCREMENTAL :
		state->coordinate_mode = DRILL_MODE_INCREMENTAL;
		break;
	    case DRILL_G_ZEROSET :
		if((read = gerb_fgetc(fd)) == EOF)
		drill_stats_add_error(stats->error_list,
				      -1,
				      "Unexpected EOF found.\n",
				      GRB_ERROR);
		drill_parse_coordinate(fd, (char)read, image, state);
		state->origin_x = state->curr_x;
		state->origin_y = state->curr_y;
		break;
	    default :
		eat_line(fd);
		break;
	    }
	    break;
       case 'I':
	   if (state->curr_section != DRILL_HEADER) break;
	   {int c = gerb_fgetc(fd);
	       switch (c) {
	       case 'N':
		   if ('C' == gerb_fgetc(fd))
		   if ('H' == gerb_fgetc(fd)) {
		       state->unit = INCH;

		       /* Look for TZ/LZ */
		       if (',' == gerb_fgetc(fd)) {
			   c = gerb_fgetc(fd);
			   if (c != EOF && 'Z' == gerb_fgetc(fd)) {
			       switch (c) {
			       case 'L':
				   if (state->autod)
				       {
					   image->format->omit_zeros = TRAILING;
					   state->header_number_format =
					       state->number_format = FMT_00_0000;
					   state->decimals = 4;
				       }
				   break;

			       case 'T':
				   if (state->autod)
				       {
					   image->format->omit_zeros = LEADING;
					   state->header_number_format =
					       state->number_format = FMT_00_0000;
					   state->decimals = 4;
				       }
				   break;

			       default:
				   drill_stats_add_error(stats->error_list,
							 -1,
							 "Found junk after INCH command\n",
							 WARNING);
				   break;
			       }
			   } else {
				   drill_stats_add_error(stats->error_list,
							 -1,
							 "Found junk after INCH command\n",
							 WARNING);
			   }
		       }
		   }
		   break;
	       case 'C':
		   if ('I' == gerb_fgetc(fd))
		   if (',' == gerb_fgetc(fd))
		   if ('O' == gerb_fgetc(fd)) {
		       if ('N' == (c = gerb_fgetc(fd)))
			   state->coordinate_mode = DRILL_MODE_INCREMENTAL;
		       else if ('F' == c) if ('F' == gerb_fgetc(fd))
			   state->coordinate_mode = DRILL_MODE_ABSOLUTE;
		   }
		   break;
	       }
	       eat_line(fd);
	   }
	   break;

	case 'M':
	    switch(drill_parse_M_code(fd, state, image)) {
	    case DRILL_M_HEADER :
		state->curr_section = DRILL_HEADER;
		break;
	    case DRILL_M_ENDHEADER :
		state->curr_section = DRILL_DATA;

		if (image->format->omit_zeros == ZEROS_UNSPECIFIED) {
		    /* Excellon says they default to specify leading
		       zeros, i.e. omit trailing zeros.	 The Excellon
		       files floating around that don't specify the
		       leading/trailing zeros in the header seem to
		       contradict to this though.

		       XXX We should probably ask the user. */

		    drill_stats_add_error(stats->error_list,
					  -1,
					  "End of Excellon header reached but no leading/trailing zero handling specified.\n",
					  GRB_ERROR);
		    drill_stats_add_error(stats->error_list,
					  -1,
					  "Assuming leading zeros.\n",
					  WARNING);
		    image->format->omit_zeros = LEADING;
		}
		break;
	    case DRILL_M_METRIC :
		if (state->unit == UNIT_UNSPECIFIED &&
		    state->curr_section != DRILL_HEADER) {
		    drill_stats_add_error(stats->error_list,
					  -1,
					  "M71 code found but no METRIC specification in header.\n",
					  GRB_ERROR);
		    drill_stats_add_error(stats->error_list,
					  -1,
					  "Assuming all tool sizes are MM.\n",
					  WARNING);
		    int tool_num;
		    double size;
		    stats = image->drill_stats;
		    for (tool_num = TOOL_MIN; tool_num < TOOL_MAX; tool_num++) {
			if (image->aperture && image->aperture[tool_num]) {
			    /* First update stats.   Do this before changing drill dias.
			     * Maybe also put error into stats? */
			    size = image->aperture[tool_num]->parameter[0];
			    drill_stats_modify_drill_list(stats->drill_list, 
							  tool_num, 
							  size, 
							  "MM");
			    /* Now go back and update all tool dias, since
			     * tools are displayed in inch units
			     */
			    image->aperture[tool_num]->parameter[0] /= 25.4;
			}
		    }
		}
		if (state->autod)
		    {
			state->number_format = state->backup_number_format;
			state->unit = MM;
		    }
		break;
	    case DRILL_M_IMPERIAL :
		if (state->autod)
		    {
			if (state->number_format != FMT_00_0000)
			    /* save metric format definition for later */
			    state->backup_number_format = state->number_format;
			state->number_format = FMT_00_0000;
			state->decimals = 4;
			state->unit = INCH;
		    }

		break;
	    case DRILL_M_LONGMESSAGE :
	    case DRILL_M_MESSAGE :
	    case DRILL_M_CANNEDTEXT :
		tmps = get_line(fd);
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Message embedded in drill file: '%s'\n", 
						      tmps),
				      NOTE);
		g_free (tmps);
		break;
	    case DRILL_M_NOT_IMPLEMENTED :
	    case DRILL_M_ENDPATTERN :
	    case DRILL_M_TIPCHECK :
		break;
	    case DRILL_M_END :
		/* M00 has optional arguments */
		eat_line(fd);
	    case DRILL_M_ENDREWIND :
		goto drill_parse_end;
		break;
	    case DRILL_M_METRICHEADER :
	      state->unit = MM;
	      break;
	    default:
		drill_stats_add_error(stats->error_list,
				      -1,
				      "Undefined M code found.\n",
				      GRB_ERROR);
	    }
	    break;

	case 'S':
	    drill_stats_add_error(stats->error_list,
				  -1,
				  "Drill file sets spindle speed -- ignoring.\n",
				  NOTE);
	    eat_line(fd);
	    break;
	case 'T':
	    drill_parse_T_code(fd, state, image);
	    break;
	case 'V' :
	    gerb_ungetc (fd);
	    tmps = get_line (fd);
	    /* Silently ignore VER,1.  Not sure what others are allowed */
	    if (strcmp (tmps, "VER,1") != 0) {
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Undefined header line = '%s'\n",tmps),
				      NOTE);
	    }
	    g_free (tmps);
	    break;

	case 'X':
	case 'Y':
	    /* Hole coordinate found. Do some parsing */
	    drill_parse_coordinate(fd, read, image, state);

	    /* Add one to drill stats  for the current tool */
	    drill_stats_increment_drill_counter(image->drill_stats->drill_list,
						state->current_tool);

	    curr_net->next = (gerb_net_t *)g_malloc(sizeof(gerb_net_t));
	    if (curr_net->next == NULL)
		GERB_FATAL_ERROR("malloc curr_net->next failed\n");
	    curr_net = curr_net->next;
	    memset((void *)curr_net, 0, sizeof(gerb_net_t));
   	    curr_net->layer = image->layers;
	    curr_net->state = image->states;
	    curr_net->start_x = (double)state->curr_x;
	    curr_net->start_y = (double)state->curr_y;
	    /* KLUDGE. This function isn't allowed to return anything
	       but inches */
	    if(state->unit == MM) {
		curr_net->start_x /= 25.4;
		curr_net->start_y /= 25.4;
		/* KLUDGE. All images, regardless of input format,
		   are returned in INCH format */
		curr_net->state->unit = INCH;
	    }

	    curr_net->stop_x = curr_net->start_x - state->origin_x;
	    curr_net->stop_y = curr_net->start_y - state->origin_y;
	    curr_net->aperture = state->current_tool;
	    curr_net->aperture_state = FLASH;

	    /* Find min and max of image.
	       Mustn't forget (again) to add the hole radius */

	    /* Check if aperture is set. Ignore the below instead of
	       causing SEGV... */
	    if(image->aperture[state->current_tool] == NULL)
		break;

	    image->info->min_x =
		min(image->info->min_x,
		    (curr_net->start_x -
		     image->aperture[state->current_tool]->parameter[0] / 2));
	    image->info->min_y =
		min(image->info->min_y,
		    (curr_net->start_y -
		     image->aperture[state->current_tool]->parameter[0] / 2));
	    image->info->max_x =
		max(image->info->max_x,
		    (curr_net->start_x +
		     image->aperture[state->current_tool]->parameter[0] / 2));
	    image->info->max_y =
		max(image->info->max_y,
		    (curr_net->start_y +
		     image->aperture[state->current_tool]->parameter[0] / 2));
	    break;
	case '%':
	    state->curr_section = DRILL_DATA;
	    break;
	case 10 :   /* White space */
	case 13 :
	case ' ' :
	case '\t' :
	    break;
	default:
	    if(state->curr_section == DRILL_HEADER) {
		/* Unrecognised crap in the header is thrown away */
		drill_stats_add_error(stats->error_list,
				      -1,
				      "Undefined codes found in header.\n",
				      GRB_ERROR);
		gerb_ungetc(fd);
		tmps = get_line(fd);
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Undefined header line = '%s'\n",
						      tmps),
				      NOTE);
		g_free (tmps);
	    } else {
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Undefined character '%c' [0x%02x] found inside data, ignoring\n",
						      read, read),
				      GRB_ERROR);
	    }
	}
    }
    drill_stats_add_error(stats->error_list,
			  -1,
			  "No EOF found in drill file.\n",
			  GRB_ERROR);

 drill_parse_end:
    dprintf ("%s():  Populating file attributes\n", __FUNCTION__);

    switch (state->unit)
	{
	case MM:
	    image->info->attr_list[HA_xy_units].default_val.int_value = UNITS_MM;
	    /* image->info->attr_list[HA_tool_units].default_val.int_value = UNITS_MM; */
	    break;

	default:
	    image->info->attr_list[HA_xy_units].default_val.int_value = UNITS_INCH;
	    /* image->info->attr_list[HA_tool_units].default_val.int_value = UNITS_INCH; */
	    break;
	}

    switch (state->number_format)
	{
	case FMT_000_00:
	case FMT_0000_00:
	    image->info->attr_list[HA_digits].default_val.int_value = 2;
	    break;

	case FMT_000_000:
	    image->info->attr_list[HA_digits].default_val.int_value = 3;
	    break;

	case FMT_00_0000:
	    image->info->attr_list[HA_digits].default_val.int_value = 4;
	    break;

	case FMT_USER:
	    dprintf ("%s():  Keeping user specified number of decimal places (%d)\n",
		     __FUNCTION__,
		     image->info->attr_list[HA_digits].default_val.int_value);
	    break;

	default:
	    break;
	}

    switch (image->format->omit_zeros)
	{
	case LEADING:
	    image->info->attr_list[HA_supression].default_val.int_value = SUP_LEAD;
	    break;
	    
	case TRAILING:
	    image->info->attr_list[HA_supression].default_val.int_value = SUP_TRAIL;
	    break;

	default:
	    image->info->attr_list[HA_supression].default_val.int_value = SUP_NONE;
	    break;
	}

    g_free(state);

    return image;
} /* parse_drillfile */


/* -------------------------------------------------------------- */
/*
 * Checks for signs that this is a drill file
 * Returns TRUE if it is, FALSE if not.
 */
gboolean
drill_file_p(gerb_file_t *fd, gboolean *returnFoundBinary)
{
  char *buf;
  int len = 0;
  char *letter;
  int ascii;
  int zero = 48; /* ascii 0 */
  int nine = 57; /* ascii 9 */
  int i;
  gboolean found_binary = FALSE;
  gboolean found_M48 = FALSE;
  gboolean found_M30 = FALSE;
  gboolean found_percent = FALSE;
  gboolean found_T = FALSE;
  gboolean found_X = FALSE;
  gboolean found_Y = FALSE;
 
  buf = g_malloc(MAXL);
  if (buf == NULL) 
    GERB_FATAL_ERROR("malloc buf failed while checking for drill file.\n");

  while (fgets(buf, MAXL, fd->fd) != NULL) {
    len = strlen(buf);

    /* First look through the file for indications of its type */

    /* check that file is not binary (non-printing chars) */
    for (i = 0; i < len; i++) {
      ascii = (int) buf[i];
      if ((ascii > 128) || (ascii < 0)) {
        found_binary = TRUE;
      }
    }

    /* Check for M48 = start of drill header */
    if (g_strstr_len(buf, len, "M48")) {
	  found_M48 = TRUE; 
    }

    /* Check for M30 = end of drill program */
    if (g_strstr_len(buf, len, "M30")) {
	if (found_percent) {
	  found_M30 = TRUE; /* Found M30 after % = good */
	}
    }

    /* Check for % on its own line at end of header */
    if ((letter = g_strstr_len(buf, len, "%")) != NULL) {
      if ((letter[1] ==  '\r') || (letter[1] ==  '\n'))
	found_percent = TRUE;
    }

    /* Check for T<number> */
    if ((letter = g_strstr_len(buf, len, "T")) != NULL) {
      if (!found_T && (found_X || found_Y)) {
	found_T = FALSE;  /* Found first T after X or Y */
      } else {
	if (isdigit( (int) letter[1])) { /* verify next char is digit */
	  found_T = TRUE;
	}
      }
    }

    /* look for X<number> or Y<number> */
    if ((letter = g_strstr_len(buf, len, "X")) != NULL) {
      ascii = (int) letter[1]; /* grab char after X */
      if ((ascii >= zero) && (ascii <= nine)) {
	found_X = TRUE;
      }
    }
    if ((letter = g_strstr_len(buf, len, "Y")) != NULL) {
      ascii = (int) letter[1]; /* grab char after Y */
      if ((ascii >= zero) && (ascii <= nine)) {
	found_Y = TRUE;
      }
    }
  } /* while (fgets(buf, MAXL, fd->fd) */

  rewind(fd->fd);
  free(buf);
  *returnFoundBinary = found_binary;

  /* Now form logical expression determining if this is a drill file */
  if ( ((found_X || found_Y) && found_T) && 
       (found_M48 || (found_percent && found_M30))
     ) 
    return TRUE;
  else if (found_M48 && found_T && found_percent && found_M30)
      /* Pathological case of drill file with valid header 
	 and EOF but no drill XY locations. */
    return TRUE;
  else 
    return FALSE;
}

/* -------------------------------------------------------------- */
/* Parse tool definition. This can get a bit tricky since it can
   appear in the header and/or data section.
   Returns tool number on success, -1 on error */
static int
drill_parse_T_code(gerb_file_t *fd, drill_state_t *state, gerb_image_t *image)
{
    int tool_num;
    gboolean done = FALSE;
    int temp;
    double size;
    drill_stats_t *stats = image->drill_stats;
    char *tmps;

    /* Sneak a peek at what's hiding after the 'T'. Ugly fix for
       broken headers from Orcad, which is crap */
    temp = gerb_fgetc(fd);
    dprintf("Found a char %d after the T\n", temp);
    if( !(isdigit(temp) != 0 || temp == '+' || temp =='-') ) {
	if(temp != EOF) {
	    drill_stats_add_error(stats->error_list,
				  -1,
				  "Orcad bug: Junk text found in place of tool definition.\n",
				  GRB_ERROR);
	    tmps = get_line(fd);

	    drill_stats_add_error(stats->error_list,
				  -1,
				  g_strdup_printf("Junk text = %s\n", 
						  tmps),
				  NOTE);
	    g_free (tmps);
	    drill_stats_add_error(stats->error_list,
				  -1,
				  "Ignorning junk text.\n",
				  WARNING);
	}
	return -1;
    }
    gerb_ungetc(fd);

    tool_num = (int) gerb_fgetint(fd, NULL);
    dprintf ("In %s: handling tool_num = %d\n", __FUNCTION__, tool_num);

    if (tool_num == 0) 
	return tool_num; /* T00 is a command to unload the drill */

    if ( (tool_num < TOOL_MIN) || (tool_num >= TOOL_MAX) ) {
	drill_stats_add_error(stats->error_list,
			      -1,
			      g_strdup_printf("Drill number out of bounds: %d.\n", tool_num),
			      GRB_ERROR);
    }

    /* Set the current tool to the correct one */
    state->current_tool = tool_num;

    /* Check for a size definition */
    temp = gerb_fgetc(fd);

    /* This bit of code looks for a tool definition by scanning for strings
     * of form TxxC, TxxF, TxxS.  */
    while(!done) {
	
	switch((char)temp) {
	case 'C':
	    size = read_double(fd, state->header_number_format, TRAILING, state->decimals);
	    dprintf ("%s: Read a size of %g %s\n", __FUNCTION__, size,
		     state->unit == MM ? "mm" : "inch");
	    if(state->unit == MM) {
		size /= 25.4;
	    } else if(size >= 4.0) {
		/* If the drill size is >= 4 inches, assume that this
		   must be wrong and that the units are mils.
		   The limit being 4 inches is because the smallest drill
		   I've ever seen used is 0,3mm(about 12mil). Half of that
		   seemed a bit too small a margin, so a third it is */
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Read a drill of diameter %g inches.\n", size),
				      GRB_ERROR); 
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Assuming units are mils.\n"),
				      WARNING); 
		size /= 1000.0;
	    }

	    if(size <= 0. || size >= 10000.) {
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Unreasonable drill size found for drill %d: %g\n", tool_num, size),
				      GRB_ERROR);
	    } else {
		if(image->aperture[tool_num] != NULL) {
		    /* allow a redefine of a tool only if the new definition is exactly the same.
		     * This avoid lots of spurious complaints with the output of some cad
		     * tools while keeping complaints if there is a true problem
		     */
		    if (image->aperture[tool_num]->parameter[0] != size ||
			image->aperture[tool_num]->type != CIRCLE ||
			image->aperture[tool_num]->nuf_parameters != 1 ||
			image->aperture[tool_num]->unit != INCH) {
			drill_stats_add_error(stats->error_list,
					      -1,
					      g_strdup_printf("Found redefinition of drill %d.\n", tool_num),
					      GRB_ERROR);
		    }
		} else {
		    image->aperture[tool_num] =
			(gerb_aperture_t *)g_malloc(sizeof(gerb_aperture_t));
		    if (image->aperture[tool_num] == NULL) {
			GERB_FATAL_ERROR("malloc tool failed\n");
		    }
		    /* make sure we zero out all aperature parameters */
		    memset((void *)image->aperture[tool_num], 0, sizeof(gerb_aperture_t));
		    /* There's really no way of knowing what unit the tools
		       are defined in without sneaking a peek in the rest of
		       the file first. That's done in drill_guess_format() */
		    image->aperture[tool_num]->parameter[0] = size;
		    image->aperture[tool_num]->type = CIRCLE;
		    image->aperture[tool_num]->nuf_parameters = 1;
		    image->aperture[tool_num]->unit = INCH;
		}
	    }
	    
	    /* Add the tool whose definition we just found into the list
	     * of tools for this layer used to generate statistics. */
	    stats = image->drill_stats;
	    drill_stats_add_to_drill_list(stats->drill_list, 
					  tool_num, 
					  size, 
					  g_strdup_printf("%s", (state->unit == MM ? "mm" : "inch")));

	    break;

	case 'F':
	case 'S' :
	    /* Silently ignored. They're not important. */
	    gerb_fgetint(fd, NULL);
	    break;

	default:
	    /* Stop when finding anything but what's expected
	       (and put it back) */
	    gerb_ungetc(fd);
	    done = TRUE;
	    break;
	}  /* switch((char)temp) */

	if( (temp = gerb_fgetc(fd)) == EOF) {
	    drill_stats_add_error(stats->error_list,
				  -1,
				  "Unexpected EOF encountered header of drill file.\n",
				  GRB_ERROR);
	}
    }   /* while(!done) */  /* Done looking at tool definitions */

    /* Catch the tools that aren't defined.
       This isn't strictly a good thing, but at least something is shown */
    if(image->aperture[tool_num] == NULL) {
        double dia;

	image->aperture[tool_num] =
	    (gerb_aperture_t *)g_malloc(sizeof(gerb_aperture_t));
	if (image->aperture[tool_num] == NULL) {
	    GERB_FATAL_ERROR("malloc tool failed\n");
	}
	/* make sure we zero out all aperature parameters */
	memset((void *)image->aperture[tool_num], 0, sizeof(gerb_aperture_t));

        /* See if we have the tool table */
        dia = GetToolDiameter_Inches(tool_num);
        if (dia <= 0) {
            /*
             * There is no tool. So go out and make some.
             * This size calculation is, of course, totally bogus.
             */
            dia = (double)(16 + 8 * tool_num) / 1000;
            /*
             * Oooh, this is sooo ugly. But some CAD systems seem to always
             * use T00 at the end of the file while others that don't have
             * tool definitions inside the file never seem to use T00 at all.
             */
            if(tool_num != 0) {
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Tool %02d used without being defined\n", tool_num),
				      GRB_ERROR);
		drill_stats_add_error(stats->error_list,
				      -1,
				      g_strdup_printf("Setting a default size of %g\"\n", dia),
				      WARNING);
            }
	}

	image->aperture[tool_num]->type = CIRCLE;
	image->aperture[tool_num]->nuf_parameters = 1;
	image->aperture[tool_num]->parameter[0] = dia;

	/* Add the tool whose definition we just found into the list
	 * of tools for this layer used to generate statistics. */
	if (tool_num != 0) {  /* Only add non-zero tool nums.  
			       * Zero = unload command. */
	    stats = image->drill_stats;
	    drill_stats_add_to_drill_list(stats->drill_list, 
					  tool_num, 
					  dia, 
					  g_strdup_printf("%s", 
							  (state->unit == MM ? "mm" : "inch")));
	}
    } /* if(image->aperture[tool_num] == NULL) */	
    
    return tool_num;
} /* drill_parse_T_code */


/* -------------------------------------------------------------- */
static int
drill_parse_M_code(gerb_file_t *fd, drill_state_t *state, gerb_image_t *image)
{
    char op[3] = "  ";
    int  read[3];
    drill_stats_t *stats = image->drill_stats;
    int result=0;

    dprintf("---> entering drill_parse_M_code ...\n");

    read[0] = gerb_fgetc(fd);
    read[1] = gerb_fgetc(fd);

    if ((read[0] == EOF) || (read[1] == EOF))
	drill_stats_add_error(stats->error_list,
			      -1,
			      "Unexpected EOF found while parsing M code.\n",
			      GRB_ERROR);
    op[0] = read[0], op[1] = read[1], op[2] = 0;
 
    if (strncmp(op, "00", 2) == 0) {
	stats->M00++;
	result = DRILL_M_END;
    } else if (strncmp(op, "01", 2) == 0) {
	stats->M01++;
	result = DRILL_M_ENDPATTERN;
    } else if (strncmp(op, "18", 2) == 0) {
	stats->M18++;
	result = DRILL_M_TIPCHECK;
    } else if (strncmp(op, "25", 2) == 0) {
	stats->M25++;
	result = DRILL_M_BEGINPATTERN;
    } else if (strncmp(op, "31", 2) == 0) {
	stats->M31++;
	result = DRILL_M_BEGINPATTERN;
    } else if (strncmp(op, "30", 2) == 0) {
	stats->M30++;
	result = DRILL_M_ENDREWIND;
    } else if (strncmp(op, "45", 2) == 0) {
	stats->M45++;
	result = DRILL_M_LONGMESSAGE;
    } else if (strncmp(op, "47", 2) == 0) {
	stats->M47++;
	result = DRILL_M_MESSAGE;
    } else if (strncmp(op, "48", 2) == 0) {
	stats->M48++;
	result = DRILL_M_HEADER;
    } else if (strncmp(op, "71", 2) == 0) {
	eat_line(fd);
	stats->M71++;
	result = DRILL_M_METRIC;
    } else if (strncmp(op, "72", 2) == 0) {
	eat_line(fd);
	stats->M72++;
	result = DRILL_M_IMPERIAL;
    } else if (strncmp(op, "95", 2) == 0) {
	stats->M95++;
	result = DRILL_M_ENDHEADER;
    } else if (strncmp(op, "97", 2) == 0) {
	stats->M97++;
	result = DRILL_M_CANNEDTEXT;
    } else if (strncmp(op, "98", 2) == 0) {
	stats->M98++;
	return DRILL_M_CANNEDTEXT;
    } else if (state->curr_section == DRILL_HEADER &&
	       strncmp(op, "ET", 2) == 0) {
	/* METRIC is not an actual M code but a command that is only
	   acceptable within the header.

	   The syntax is
	   METRIC[,{TZ|LZ}][,{000.000|000.00|0000.00}]
	*/
	if ('R' == gerb_fgetc(fd) &&
	    'I' == gerb_fgetc(fd) &&
	    'C' == gerb_fgetc(fd)) {
	again:
	    if (',' == gerb_fgetc(fd)) {
		int c;

		/* Is it tzlz, or zerofmt? */
		switch ((c = gerb_fgetc(fd))) {
		case 'T':
		case 'L':
		    if ('Z' != gerb_fgetc(fd))
			goto junk;
		    if (c == 'L')
			{
			    dprintf ("%s(): Detected a file that probably has trailing zero supression\n", __FUNCTION__);
			    if (state->autod)
				{
				    image->format->omit_zeros = TRAILING;
				}
			}
		    else
			{
			    dprintf ("%s(): Detected a file that probably has leading zero supression\n", __FUNCTION__);
			    if (state->autod)
				{
				    image->format->omit_zeros = LEADING;
				}
			}
		    if (state->autod)
			{
			    /* Default metric number format is 6-digit, 1 um
			       resolution.  The header number format (for T#C#
			       definitions) is fixed to that, while the number
			       format within the file can differ. */
			    state->header_number_format =
				state->number_format = FMT_000_000;
			    state->decimals = 3;
			}
		    c = gerb_fgetc(fd);
		    gerb_ungetc(fd);
		    if (c == ',')
			/* anticipate number format will follow */
			goto again;
		    break;

		case '0':
		    if ('0' != gerb_fgetc(fd) ||
			'0' != gerb_fgetc(fd))
			goto junk;
		    /* We just parsed three 0s, the remainder options
		       so far are: .000 | .00 | 0.00 */
		    read[0] = gerb_fgetc(fd);
		    read[1] = gerb_fgetc(fd);
		    if (read[0] == EOF || read[1] == EOF)
			goto junk;
		    op[0] = read[0];
		    op[1] = read[1];
		    if (strcmp(op, "0.") == 0) {
			/* expecting FMT_0000_00,
			   two trailing 0s must follow */
			if ('0' != gerb_fgetc(fd) ||
			    '0' != gerb_fgetc(fd))
			    goto junk;
			eat_line(fd);
			if (state->autod)
			    {
				state->number_format = FMT_0000_00;
				state->decimals = 2;
			    }
			break;
		    }
		    if (strcmp(op, ".0") != 0)
			goto junk;
		    /* must be either FMT_000_000 or FMT_000_00, depending
		       on whether one or two 0s are following */
		    if ('0' != gerb_fgetc(fd))
			goto junk;
		    if ('0' == gerb_fgetc(fd) && state->autod)
			{
			    state->number_format = FMT_000_000;
			    state->decimals = 3;
			}
		    else {
			gerb_ungetc(fd);
			if (state->autod)
			    {
				state->number_format = FMT_000_00;
				state->decimals = 2;
			    }
		    }
		    eat_line(fd);
		    break;

		default:
		junk:
		    drill_stats_add_error(stats->error_list,
					  -1,
					  "Found junk after METRIC command\n",
					  WARNING);
		    gerb_ungetc(fd);
		    eat_line(fd);
		    break;
		}
	    } else {
		gerb_ungetc(fd);
		eat_line(fd);
	    }

	    return DRILL_M_METRICHEADER;
	}
    } else {
	stats->M_unknown++;
	result = DRILL_M_UNKNOWN;
    }

    dprintf("<----  ...leaving drill_parse_M_code.\n");
    return result;
} /* drill_parse_M_code */


/* -------------------------------------------------------------- */
static int
drill_parse_G_code(gerb_file_t *fd, gerb_image_t *image)
{
    char op[3] = "  ";
    int  read[3];
    drill_stats_t *stats = image->drill_stats;
    int result;
    
    dprintf("---> entering drill_parse_G_code ...\n");

    read[0] = gerb_fgetc(fd);
    read[1] = gerb_fgetc(fd);

    if ((read[0] == EOF) || (read[1] == EOF))
	drill_stats_add_error(stats->error_list,
			      -1,
			      "Unexpected EOF found while parsing G code.\n",
			      GRB_ERROR);

    op[0] = read[0], op[1] = read[1], op[2] = 0;

    if (strncmp(op, "00", 2) == 0) {
	stats->G00++;
	result = DRILL_G_ROUT;
    } else if (strncmp(op, "01", 2) == 0) {
	stats->G01++;
	result = DRILL_G_LINEARMOVE;
    } else if (strncmp(op, "02", 2) == 0) {
	stats->G02++;
	result = DRILL_G_CWMOVE;
    } else if (strncmp(op, "03", 2) == 0) {
	stats->G03++;
	result = DRILL_G_CCWMOVE;
    } else if (strncmp(op, "05", 2) == 0) {
	stats->G05++;
	result = DRILL_G_DRILL;
    } else if (strncmp(op, "90", 2) == 0) {
	stats->G90++;
	result = DRILL_G_ABSOLUTE;
    } else if (strncmp(op, "91", 2) == 0) {
	stats->G91++;
	result = DRILL_G_INCREMENTAL;
    } else if (strncmp(op, "93", 2) == 0) {
	stats->G93++;
	result = DRILL_G_ZEROSET;
    } else {	
	stats->G_unknown++;
	result = DRILL_G_UNKNOWN;
    }

    dprintf("<----  ...leaving drill_parse_G_code.\n");
    return result;

} /* drill_parse_G_code */


/* -------------------------------------------------------------- */
/* Parse on drill file coordinate.
   Returns nothing, but modifies state */
static void
drill_parse_coordinate(gerb_file_t *fd, char firstchar,
		       gerb_image_t *image, drill_state_t *state)

{
    int read;

    if(state->coordinate_mode == DRILL_MODE_ABSOLUTE) {
	if(firstchar == 'X') {
	    state->curr_x = read_double(fd, state->number_format, image->format->omit_zeros, state->decimals);
	    if((read = (char)gerb_fgetc(fd)) == 'Y') {
		state->curr_y = read_double(fd, state->number_format, image->format->omit_zeros, state->decimals);
	    }
	} else {
	    state->curr_y = read_double(fd, state->number_format, image->format->omit_zeros, state->decimals);
	}
    } else if(state->coordinate_mode == DRILL_MODE_INCREMENTAL) {
	if(firstchar == 'X') {
	    state->curr_x += read_double(fd, state->number_format, image->format->omit_zeros, state->decimals);
	    if((read = (char)gerb_fgetc(fd)) == 'Y') {
		state->curr_y += read_double(fd, state->number_format, image->format->omit_zeros, state->decimals);
	    }
	} else {
	    state->curr_y += read_double(fd, state->number_format, image->format->omit_zeros, state->decimals);
	}
    }

} /* drill_parse_coordinate */


/* Allocates and returns a new drill_state structure
   Returns state pointer on success, NULL on ERROR */
static drill_state_t *
new_state(drill_state_t *state)
{
    state = (drill_state_t *)g_malloc(sizeof(drill_state_t));
    if (state != NULL) {
	/* Init structure */
	memset((void *)state, 0, sizeof(drill_state_t));
	state->curr_section = DRILL_NONE;
	state->coordinate_mode = DRILL_MODE_ABSOLUTE;
	state->origin_x = 0.0;
	state->origin_y = 0.0;
	state->unit = UNIT_UNSPECIFIED;
	state->backup_number_format = FMT_000_000; /* only used for METRIC */
	state->header_number_format = state->number_format = FMT_00_0000; /* i. e. INCH */
	state->autod = 1;
	state->decimals = 4;

    }
    return state;
} /* new_state */


/* -------------------------------------------------------------- */
/* Reads one double from fd and returns it.
   If a decimal point is found, fmt is not used. */
static double
read_double(gerb_file_t *fd, enum number_fmt_t fmt, enum omit_zeros_t omit_zeros, int decimals)
{
    int read;
    char temp[0x20];
    int i = 0, ndigits = 0;
    double result;
    gboolean decimal_point = FALSE;

    memset(temp, 0, sizeof(temp));

    read = gerb_fgetc(fd);
    while(read != EOF && i < sizeof(temp) &&
	  (isdigit(read) || read == '.' || read == ',' || read == '+' || read == '-')) {
	if(read == ',' || read == '.') decimal_point = TRUE;
	if(read == ',')
	    read = '.'; /* adjust for strtod() */
	if(isdigit(read)) ndigits++;
	temp[i++] = (char)read;
	read = gerb_fgetc(fd);
    }
    temp[i] = 0;

    gerb_ungetc(fd);
    if (decimal_point) {
	result = strtod(temp, NULL);
    } else {
	int wantdigits;
	double scale;

	/* Nothing to take care for when leading zeros are
	   omitted. */
	if (omit_zeros == TRAILING) {
	    switch (fmt) {
	    case FMT_00_0000:
	    case FMT_000_000:
	    case FMT_0000_00:
		wantdigits = 6;
		break;

	    case FMT_000_00:
		wantdigits = 5;
		break;

	    default:
		/* cannot happen, just plugs a compiler warning */
		return 0;
	    }

	    /* fill missing trailing digits */
	    while (ndigits < wantdigits) {
		temp[i++] = '0';
		ndigits++;
	    }
	    temp[i] = 0;
	}

	switch (fmt) {
	case FMT_00_0000:
	    scale = 1E-4;
	    break;

	case FMT_000_000:
	    scale = 1E-3;
	    break;

	case FMT_000_00:
	case FMT_0000_00:
	    scale = 1E-2;
	    break;

	case FMT_USER:
	    scale = pow (10.0, -1.0*decimals);
	    break;

	default:
	    /* cannot happen, just plugs a compiler warning */
	    fprintf (stderr, "%s(): Unhandled fmt ` %d\n", __FUNCTION__, fmt);
	    exit (1);
	}

	result = strtod(temp, NULL) * scale;
    }

    return result;
} /* read_double */


/* -------------------------------------------------------------- */
/* Eats all characters up to and including 
   the first one of CR or LF */
static void
eat_line(gerb_file_t *fd)
{
    int read = gerb_fgetc(fd);
    
    while(read != 10 && read != 13) {
	if (read == EOF) return;
	read = gerb_fgetc(fd);
    }
} /* eat_line */


/* -------------------------------------------------------------- */
static char *
get_line(gerb_file_t *fd)
{
    int read = gerb_fgetc(fd);
    char *retstring = "";
    char *tmps = NULL;

    while(read != 10 && read != 13) {
	if (read == EOF) return retstring;
	retstring = g_strdup_printf("%s%c", retstring, read);

	/* since g_strdup_printf allocates memory, we need to free it */
	if (tmps)  {
	    g_free (tmps);
	    tmps = NULL;
	}
	tmps = retstring;;
	read = gerb_fgetc(fd);
    }
    return retstring;
} /* get_line */
