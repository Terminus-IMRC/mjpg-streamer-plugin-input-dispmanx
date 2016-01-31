/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <linux/videodev.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <bcm_host.h>
#include <jpeglib.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "dispmanx input plugin"
#define MAX_ARGUMENTS 32

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;
static pthread_mutex_t controls_mutex;

void *worker_thread( void *);
void worker_cleanup(void *);
void help(void);

static int delay = 1000;

int displayRotated = 0;
DISPMANX_DISPLAY_HANDLE_T displayHandle;
DISPMANX_RESOURCE_HANDLE_T resourceHandle;
uint32_t vcImagePtr = 0;
int width, height;
uint8_t *image;
int pitch;
VC_RECT_T rect;

/* details of converted JPG pictures */
struct pic {
  const unsigned char *data;
  const int size;
};

/*** plugin interface functions ***/

/******************************************************************************
Description.: parse input parameters
Input Value.: param contains the command line string and a pointer to globals
Return Value: 0 if everything is ok
******************************************************************************/
int input_init(input_parameter *param) {
  char *argv[MAX_ARGUMENTS]={NULL};
  int argc=1, i;

	DISPMANX_MODEINFO_T modeInfo;
	char responce[1024];
	int displayNumber = 0;
	int ret;

  if( pthread_mutex_init(&controls_mutex, NULL) != 0 ) {
    IPRINT("could not initialize mutex variable\n");
    exit(EXIT_FAILURE);
  }

  /* convert the single parameter-string to an array of strings */
  argv[0] = INPUT_PLUGIN_NAME;
  if ( param->parameter_string != NULL && strlen(param->parameter_string) != 0 ) {
    char *arg=NULL, *saveptr=NULL, *token=NULL;

    arg=(char *)strdup(param->parameter_string);

    if ( strchr(arg, ' ') != NULL ) {
      token=strtok_r(arg, " ", &saveptr);
      if ( token != NULL ) {
        argv[argc] = strdup(token);
        argc++;
        while ( (token=strtok_r(NULL, " ", &saveptr)) != NULL ) {
          argv[argc] = strdup(token);
          argc++;
          if (argc >= MAX_ARGUMENTS) {
            IPRINT("ERROR: too many arguments to input plugin\n");
            return 1;
          }
        }
      }
    }
  }

  /* show all parameters for DBG purposes */
  for (i=0; i<argc; i++) {
    DBG("argv[%d]=%s\n", i, argv[i]);
  }

  reset_getopt();
  while(1) {
    int option_index = 0, c=0;
    static struct option long_options[] = \
    {
      {"h", no_argument, 0, 0},
      {"help", no_argument, 0, 0},
      {"d", required_argument, 0, 0},
      {"delay", required_argument, 0, 0},
      {0, 0, 0, 0}
    };

    c = getopt_long_only(argc, argv, "", long_options, &option_index);

    /* no more options to parse */
    if (c == -1) break;

    /* unrecognized option */
    if (c == '?'){
      help();
      return 1;
    }

    switch (option_index) {
      /* h, help */
      case 0:
      case 1:
        DBG("case 0,1\n");
        help();
        return 1;
        break;

      /* d, delay */
      case 2:
      case 3:
        DBG("case 2,3\n");
        delay = atoi(optarg);
        break;

      default:
        DBG("default case\n");
        help();
        return 1;
    }
  }

  pglobal = param->global;

	bcm_host_init();

	ret = vc_gencmd(responce, sizeof(responce), "get_config int");
	if (ret == 0)
		vc_gencmd_number_property(responce, "display_rotate", &displayRotated);
	printf("displayRotated: %d\n", displayRotated);

	displayHandle = vc_dispmanx_display_open(displayNumber);
	if (displayHandle == 0) {
		fprintf(stderr, "error: vc_dispmanx_display_open\n");
		exit(EXIT_FAILURE);
	}

	ret = vc_dispmanx_display_get_info(displayHandle, &modeInfo);
	if (ret != 0) {
		fprintf(stderr, "error: vc_dispmanx_display_get_info\n");
		exit(EXIT_FAILURE);
	}

	if (displayRotated&1) {
		width = modeInfo.height;
		height = modeInfo.width;
	} else {
		width = modeInfo.width;
		height = modeInfo.height;
	}

	resourceHandle = vc_dispmanx_resource_create(VC_IMAGE_RGBA32, width, height, &vcImagePtr);
	image = malloc(width * height * (32 / 8) * 4);
	if (image == NULL) {
		fprintf(stderr, "error: failed to allocate memory\n");
		exit(EXIT_FAILURE);
	}
	pitch = (32 / 8) * width;

	ret = vc_dispmanx_rect_set(&rect, 0, 0, width, height);
	if (ret != 0) {
		fprintf(stderr, "error: vc_dispmanx_rect_set: %d\n", ret);
		exit(EXIT_FAILURE);
	}

  IPRINT("delay.............: %i\n", delay);

  return 0;
}

/******************************************************************************
Description.: stops the execution of the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
int input_stop(void) {
  DBG("will cancel input thread\n");
  pthread_cancel(worker);

  return 0;
}

/******************************************************************************
Description.: starts the worker thread and allocates memory
Input Value.: -
Return Value: 0
******************************************************************************/
int input_run(void) {
  pglobal->buf = malloc(256*1024);
  if (pglobal->buf == NULL) {
    fprintf(stderr, "could not allocate memory\n");
    exit(EXIT_FAILURE);
  }

  if( pthread_create(&worker, 0, worker_thread, NULL) != 0) {
    free(pglobal->buf);
    fprintf(stderr, "could not start worker thread\n");
    exit(EXIT_FAILURE);
  }
  pthread_detach(worker);

  return 0;
}

/******************************************************************************
Description.: print help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void) {
    fprintf(stderr, " ---------------------------------------------------------------\n" \
                    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
                    " ---------------------------------------------------------------\n" \
                    " The following parameters can be passed to this plugin:\n\n" \
                    " [-d | --delay ]........: delay to pause between frames\n" \
                    " [-r | --resolution]....: can be 960x720, 640x480, 320x240, 160x120\n"
                    " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: copy a picture from testpictures.h and signal this to all output
              plugins, afterwards switch to the next frame of the animation.
Input Value.: arg is not used
Return Value: NULL
******************************************************************************/
void *worker_thread( void *arg ) {
	(void)arg;
  /* set cleanup handler to cleanup allocated ressources */
  pthread_cleanup_push(worker_cleanup, NULL);

  while( !pglobal->stop ) {
		uint8_t *encoded_image;
		unsigned long int encoded_image_size;
		struct jpeg_compress_struct cinfo;
		struct jpeg_error_mgr jerr;
		int i;
		int ret;

		ret = vc_dispmanx_snapshot(displayHandle, resourceHandle, DISPMANX_NO_ROTATE);
		if (ret != 0) {
			fprintf(stderr, "error: vc_dispmanx_snapshot: %d\n", ret);
			exit(EXIT_FAILURE);
		}

		ret = vc_dispmanx_resource_read_data(resourceHandle, &rect, image, pitch);
		if (ret != 0) {
			fprintf(stderr, "error: vc_dispmanx_resource_read_data: %d\n", ret);
			exit(EXIT_FAILURE);
		}

		cinfo.err = jpeg_std_error(&jerr);
		jpeg_create_compress(&cinfo);
		jpeg_mem_dest(&cinfo, &encoded_image, &encoded_image_size);

		cinfo.image_width = width;
		cinfo.image_height = height;
		cinfo.input_components = 4;
		cinfo.in_color_space = JCS_EXT_RGBA;
		cinfo.jpeg_color_space = JCS_RGB;

		jpeg_set_defaults(&cinfo);
		jpeg_set_quality(&cinfo, 255, 1);
		jpeg_start_compress(&cinfo, 1);
		for (i = 0; i < height; i ++) {
			uint8_t *p = ((uint8_t*)image) + i * width * 4;
			jpeg_write_scanlines(&cinfo, &p, 1);
		}
		jpeg_finish_compress(&cinfo);
		jpeg_destroy_compress(&cinfo);


    /* copy JPG picture to global buffer */
    pthread_mutex_lock( &pglobal->db );

		pglobal->buf=encoded_image;
		pglobal->size=encoded_image_size;

    /* signal fresh_frame */
    pthread_cond_broadcast(&pglobal->db_update);
    pthread_mutex_unlock( &pglobal->db );

    usleep(1000*delay);
  }

  IPRINT("leaving input thread, calling cleanup function now\n");
  pthread_cleanup_pop(1);

  return NULL;
}

/******************************************************************************
Description.: this functions cleans up allocated ressources
Input Value.: arg is unused
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg) {
  static unsigned char first_run=1;

	(void)arg;

  if ( !first_run ) {
    DBG("already cleaned up ressources\n");
    return;
  }

  first_run = 0;
  DBG("cleaning up ressources allocated by input thread\n");

	vc_dispmanx_resource_delete(resourceHandle);
	vc_dispmanx_display_close(displayHandle);
	bcm_host_deinit();

	return;
}

#define ONE_DEGREE (64);
#define MAX_PAN  (70*64)
#define MIN_PAN  (-70*64)
#define MAX_TILT (30*64)
#define MIN_TILT (-30*64)

int input_cmd(in_cmd_type cmd, int value) {
  int res=0;
  static int pan=0;
  static int tilt=0;
  static int pan_tilt_valid=-1;
  const int one_degree = ONE_DEGREE;

  IPRINT("received command %d (value: %d) for input plugin\n", cmd, value);

  if ( cmd != IN_CMD_RESET_PAN_TILT_NO_MUTEX )
    pthread_mutex_lock( &controls_mutex );

  DBG("pan: %d, tilt: %d, valid: %d\n", pan, tilt, pan_tilt_valid);

  switch (cmd) {
    case IN_CMD_HELLO:
      fprintf(stderr, "Hello from input plugin\n");
      break;

    case IN_CMD_RESET_PAN_TILT:
    case IN_CMD_RESET_PAN_TILT_NO_MUTEX:
      DBG("about to set all pan/tilt to default position\n");
      DBG("uvcPanTilt(videoIn, 0, 0, 3) != 0 )\n");
      pan_tilt_valid = 1;
      pan = tilt = 0;
      break;

    case IN_CMD_PAN_SET:
      DBG("set pan to %d\n", value);

      if ( pan_tilt_valid != 1 ) input_cmd(IN_CMD_RESET_PAN_TILT_NO_MUTEX, 0);

      /* limit pan-value to min and max */
      value = MIN(MAX(value*one_degree, MIN_PAN), MAX_PAN);

      /* calculate the relative degrees to move to the desired absolute pan-value */
      if( (res = value - pan) == 0 ) {
        /* do not move if this would mean to move by 0 degrees */
        res = pan;
        break;
      }

      /* move it */
      pan = value;
      DBG("res = uvcPanTilt(videoIn, %d, 0, 0)\n", res);

      DBG("pan: %d\n", pan);
      break;

    case IN_CMD_PAN_PLUS:
      DBG("pan +\n");

      if ( pan_tilt_valid != 1 ) input_cmd(IN_CMD_RESET_PAN_TILT_NO_MUTEX, 0);

      if ( (MAX_PAN) > (pan+one_degree) ) {
        pan += one_degree;
        DBG("res = uvcPanTilt(videoIn, one_degree, 0, 0)\n");
      }
      DBG("pan: %d\n", pan);
      break;

    case IN_CMD_PAN_MINUS:
      DBG("pan -\n");

      if ( pan_tilt_valid != 1 ) input_cmd(IN_CMD_RESET_PAN_TILT_NO_MUTEX, 0);

      if ( (MIN_PAN) < (pan-one_degree) ) {
        pan -= one_degree;
        DBG("res = uvcPanTilt(videoIn, -one_degree, 0, 0)\n");
      }
      DBG("pan: %d\n", pan);
      break;

    default:
      DBG("some other value, ignored\n");
  }

  if ( cmd != IN_CMD_RESET_PAN_TILT_NO_MUTEX )
    pthread_mutex_unlock( &controls_mutex );

  return res;
}




