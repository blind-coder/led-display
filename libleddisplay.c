/*
 * Copyright (C) 2009 David Ingram
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <usb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "libleddisplay.h"

// fonts
#include "led_font_time.h"
#include "led_font_std.h"

// USB Vendor and Product IDs (obtained via lsusb)
#define DEVICE_VID 0x1d34
#define DEVICE_PID 0x0013

// USB device info
static usb_dev_handle *udev;

// display brightness: 0-2, with 2 being brightest
static unsigned char _brightness = LDISPLAY_BRIGHT;

// current buffer
static ldisplay_buffer_t _buffer;

// animation thread
static pthread_t anim_thread;

// flag to kill animation
static int die_anim_thread = 0;

static ldisplay_animq_t animq;
static pthread_mutex_t  animq_mutex;

static int _ldisplay_update(void);
static ldisplay_frame_t *_ldisplay_dequeue(void);
static void _ldisplay_queue_prepend(ldisplay_frame_t *frame);

/******************************************************************************/

// internal USB functions

// send a control message to the device
static inline int _control_msg(char message[], int length) {
  // details from sniffing USB traffic:
  //   request type: 0x21
  //   request: 0x09
  //   index: 0x0200
  //   value: 0x0000
	return usb_control_msg(udev, 0x21, 0x09, 0x0200, 0x0000, message, length, 1000);
}

/*******************************************************************************/

static inline uint32_t _swapbits( uint32_t v )
{
  const uint32_t h_mask_1 = 0xaaaaaaaaUL;
  const uint32_t l_mask_1 = 0x55555555UL;

  const uint32_t h_mask_2 = 0xccccccccUL;
  const uint32_t l_mask_2 = 0x33333333UL;

  const uint32_t h_mask_4 = 0xf0f0f0f0UL;
  const uint32_t l_mask_4 = 0x0f0f0f0fUL;

  v =    ( ( v & h_mask_1 ) >> 1 ) | ( ( v & l_mask_1 ) << 1 );
  v =    ( ( v & h_mask_2 ) >> 2 ) | ( ( v & l_mask_2 ) << 2 );
  return ( ( v & h_mask_4 ) >> 4 ) | ( ( v & l_mask_4 ) << 4 );
}

static inline void _overlay(const uint32_t *foreground, uint32_t background[7], char xOff, char yOff) {
  int i;
  // index bounds checking
  if (yOff<-6 || yOff>6 || xOff<-20 || xOff>20) return;

  if (xOff<0) {
    xOff = -xOff;
    for (i= (yOff>0) ? yOff : 0; i<7; ++i) {
      if (i-yOff < 0 || i-yOff>6) continue;
      background[i] |= (uint32_t)(foreground[i-yOff] << xOff);
    }
  } else {
    for (i= (yOff>0) ? yOff : 0; i<7; ++i) {
      if (i-yOff < 0 || i-yOff>6) continue;
      background[i] |= (uint32_t)(foreground[i-yOff] >> xOff);
    }
  }
}

/*******************************************************************************/

static void _ldisplay_setBrightness(unsigned char brightness) {
  if (brightness != LDISPLAY_NOCHANGE) {
    if (brightness > LDISPLAY_BRIGHT) {
      _brightness = LDISPLAY_BRIGHT;
    } else {
      _brightness = brightness;
    }
  }
}

static void _ldisplay_reset(void) {
  int i=0;

  for (i=0; i<7; ++i) {
    _buffer[i] = 0;
  }
}

static void _ldisplay_invert(void) {
  int i=0;

  for (i=0; i<7; ++i) {
    _buffer[i] ^= 0xffffffff;
  }
}

static void _ldisplay_set(ldisplay_buffer_t data) {
  int i=0;

  for (i=0; i<7; ++i) {
    _buffer[i] = data[i];
  }
}

static void _anim_frame_dispatch(ldisplay_frame_t *frame) {
  switch (frame->type) {
    case LDISPLAY_INVERT:
      // invert the internal buffer
      _ldisplay_setBrightness(frame->brightness);
      _ldisplay_invert();
      break;
    case LDISPLAY_CLEAR:
      // clear the internal buffer
      _ldisplay_setBrightness(frame->brightness);
      _ldisplay_reset();
      break;
    case LDISPLAY_SET:
      // copy to internal buffer
      _ldisplay_setBrightness(frame->brightness);
      _ldisplay_set(frame->data.buffer);
      break;
    case LDISPLAY_LOOP:
    case LDISPLAY_BRK_IF_LAST:
    case LDISPLAY_NOOP:
    default:
      // do nothing
      break;
  }
}

static void *anim_thread_func(void *arg) {
  struct timespec    ts;
  pthread_mutex_t    mutex;
  pthread_cond_t     cond;
  ldisplay_frame_t  *cur_frame;

  pthread_mutex_init(&mutex, NULL);
  pthread_cond_init(&cond, NULL);

  while (!die_anim_thread) {
    (void)pthread_mutex_lock(&mutex);

    cur_frame = _ldisplay_dequeue();
    if (!cur_frame) {
      cur_frame = calloc(1, sizeof(ldisplay_frame_t));
      cur_frame->duration = MAX_FRAME_LENGTH_MS;
      cur_frame->brightness = LDISPLAY_NOCHANGE;
      cur_frame->type = LDISPLAY_NOOP;
      printf("Creating fake NOOP frame with duration %d\n", cur_frame->duration);
    }
    if (cur_frame->duration > MAX_FRAME_LENGTH_MS) {
      cur_frame->duration -= MAX_FRAME_LENGTH_MS;
      ldisplay_frame_t *tmp = calloc(1, sizeof(ldisplay_frame_t));
      memcpy(tmp, cur_frame, sizeof(ldisplay_frame_t));
      _ldisplay_queue_prepend(tmp);
      cur_frame->duration = MAX_FRAME_LENGTH_MS;
    }

    // set up delay
    int rc = 0;
    do {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += cur_frame->duration * 1e6; // ms -> ns
      while (rc == 0) {
        rc = pthread_cond_timedwait(&cond, &mutex, &ts);
      }
      if (rc == ETIMEDOUT) {
        break;
      }
      printf("Redoing; tv_sec: %ld; tv_nsec: %ld\n", ts.tv_sec, ts.tv_nsec);
    } while (rc == EINVAL);

    // handle frame
    _anim_frame_dispatch(cur_frame);

    // update hardware from buffer
    _ldisplay_update();

    // free the current frame
    free(cur_frame);
    cur_frame = NULL;

    (void)pthread_mutex_unlock(&mutex);
  }

  pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&mutex);

  return 0;
}



// attempt to open the first device matching the VID/PID we have
// assignes a usb_dev_handle to the device to the udev global var
int ldisplay_init() {
#ifndef NODEV
  struct usb_bus *bus;
  struct usb_device *dev;

	usb_init();
	usb_set_debug(0);
	usb_find_busses();
	usb_find_devices();

	// scan the available busses
	for (bus = usb_get_busses(); bus; bus = bus->next) {
		// scan the devices on this bus
		for (dev = bus->devices; dev; dev = dev->next) {
			// Try to open the device.
      usb_dev_handle *tdev = usb_open(dev);

			if (tdev) {
				if ((dev->descriptor.idVendor == DEVICE_VID) && (dev->descriptor.idProduct == DEVICE_PID)) {
					// save the device
					udev=tdev;

					// detach from the kernel if we need to
          char dname[32]={0};
					int retval = usb_get_driver_np(udev, 0, dname, 31);
					if (retval == 0 && strcmp(dname, "usbhid") == 0) {
						usb_detach_kernel_driver_np(udev, 0);
					}
					// Set configuration 1,interface 0,altinterface 0.
					usb_set_configuration(udev, 1);
					usleep(100);
					usb_claim_interface(udev, 0);

#else
  printf("\033[H\033[2J");
#endif
          pthread_create(&anim_thread, NULL, &anim_thread_func, NULL);

          // done
					return SUCCESS;
#ifndef NODEV
				} else {
					usb_close(tdev);
				}
			}
		}
	}

	return ERR_INIT_NODEV;
#endif
}

static int _ldisplay_update_hw(void) {
  unsigned char msg[] = { _brightness, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

  for (msg[1]=0; msg[1]<7; msg[1]+=2) {
    int i=0;
    msg[4] = ~((_buffer[msg[1]  ] & 0x00ffffff) >> 0x0d);
    msg[3] = ~((_buffer[msg[1]  ] & 0x0000ffff) >> 0x05);
    msg[2] = ~((_buffer[msg[1]  ] & 0x000000ff) << 0x03);
    if (msg[1]<6) {
      msg[7] = ~((_buffer[msg[1]+1] & 0x00ffffff) >> 0x0d);
      msg[6] = ~((_buffer[msg[1]+1] & 0x0000ffff) >> 0x05);
      msg[5] = ~((_buffer[msg[1]+1] & 0x000000ff) << 0x03) | 0x07;
    } else {
      msg[5] = 0;
      msg[6] = 0;
      msg[7] = 0;
    }
    for (i=2; i<8; ++i) {
      msg[i] = _swapbits(msg[i]);
    }

    int ret;
    if ((ret = _control_msg((char*)msg, 8)) < 0) {
      return ret;
    }
  }

  return SUCCESS;
}

static int _ldisplay_update_sim(void) {
  int i, j;
  char o;

  switch (_brightness) {
    case LDISPLAY_DIM   : o='o'; break;
    case LDISPLAY_MEDIUM: o='*'; break;
    case LDISPLAY_BRIGHT: o='#'; break;
    default:              o='@'; break;
  }

  printf("\033[H");
  for (i=0; i<7; ++i) {
    for (j=21; j>=0; --j) {
      printf( "%c", ((_buffer[i] >> j) & 0x1) ? o : ' ' );
    }
    printf("|\n");
  }
  for (j=21; j>=0; --j) {
    printf("-");
  }
  printf("+\n");

  static uint16_t updcount=0;
  printf("%d\n", ++updcount);

  return SUCCESS;
}

static int _ldisplay_update(void) {
#ifndef NODEV
  return _ldisplay_update_hw();
#else
  //return _ldisplay_update_sim();
  return SUCCESS;
#endif
}


/* ************************************************************************ */

void ldisplay_enqueue(ldisplay_frame_t *frame) {
  (void)pthread_mutex_lock(&animq_mutex);
  if (animq.last) {
    animq.last->next = frame;
    animq.last = frame;
  } else {
    animq.first = frame;
    animq.last = frame;
  }
  (void)pthread_mutex_unlock(&animq_mutex);
}

static void _ldisplay_queue_prepend(ldisplay_frame_t *frame) {
  (void)pthread_mutex_lock(&animq_mutex);
  if (animq.first) {
    frame->next = animq.first;
    animq.first = frame;
  } else {
    animq.first = frame;
    animq.last = frame;
  }
  (void)pthread_mutex_unlock(&animq_mutex);
}

static ldisplay_frame_t *_ldisplay_dequeue(void) {
  ldisplay_frame_t *cur;
  (void)pthread_mutex_lock(&animq_mutex);
  if (!animq.first) {
    cur = NULL;
  } else {
    cur = animq.first;
    if (animq.first == animq.last) {
      animq.first = animq.last = NULL;
    } else {
      animq.first = animq.first->next;
    }
    printf("Dequeued frame type %d; duration %d\n", cur->type, cur->duration);
  }
  (void)pthread_mutex_unlock(&animq_mutex);
  return cur;
}

/* ************************************************************************ */


ldisplay_frame_t *_ldisplay_make_frame(unsigned char type, uint16_t duration, unsigned char brightness) {
  ldisplay_frame_t *tmp = calloc(1, sizeof(ldisplay_frame_t));

  tmp->duration = duration;
  tmp->type = type;
  tmp->brightness = brightness;

  return tmp;
}

void ldisplay_reset(uint16_t duration) {
  ldisplay_frame_t *frame = _ldisplay_make_frame(LDISPLAY_CLEAR, duration, LDISPLAY_NOCHANGE);
  ldisplay_enqueue(frame);
}

void ldisplay_invert(uint16_t duration) {
  ldisplay_frame_t *frame = _ldisplay_make_frame(LDISPLAY_INVERT, duration, LDISPLAY_NOCHANGE);
  ldisplay_enqueue(frame);
}

void ldisplay_set(uint16_t duration, ldisplay_buffer_t buffer, unsigned char brightness) {
  ldisplay_frame_t *frame = _ldisplay_make_frame(LDISPLAY_SET, duration, brightness);
  memcpy(frame->data.buffer, buffer, sizeof(ldisplay_buffer_t));
  ldisplay_enqueue(frame);
}

/*
int ldisplay_setAll(int val) {
  int i=0;

  for (i=0; i<7; ++i) {
    _buffer[i] = val ? 0xffffffff : 0;
  }

  return SUCCESS;
}

int ldisplay_setDisplay(uint32_t data[7]) {
  int i=0;

  for (i=0; i<7; ++i) {
    _buffer[i] = data[i];
  }

  return SUCCESS;
}

void ldisplay_setBrightness(unsigned char brightness) {
  if (brightness>2)
    brightness=2;
  _brightness = brightness;
}
*/

int ldisplay_drawTime(ldisplay_buffer_t buffer, unsigned int time, int style) {
  if (time>9999 || style<0 || style>1) {
    return ERR_BAD_ARGS;
  }

  CLEAR_BUFFER(buffer);

  _overlay(time_font_colon, buffer, 0, 0);

  if (style) {
    _overlay(time_segment_font_digits[(time     )%10], buffer,   0, 0);
    _overlay(time_segment_font_digits[(time/10  )%10], buffer, - 5, 0);
    _overlay(time_segment_font_digits[(time/100 )%10], buffer, -12, 0);
    _overlay(time_segment_font_digits[(time/1000)%10], buffer, -17, 0);
  } else {
    _overlay(time_font_digits[(time     )%10], buffer,   0, 0);
    _overlay(time_font_digits[(time/10  )%10], buffer, - 5, 0);
    _overlay(time_font_digits[(time/100 )%10], buffer, -12, 0);
    _overlay(time_font_digits[(time/1000)%10], buffer, -17, 0);
  }

  return SUCCESS;
}

int ldisplay_drawChars(ldisplay_buffer_t buffer, const char chars[4], char offset) {
  CLEAR_BUFFER(buffer);

  _overlay(font_std_fixed_ascii[(unsigned)chars[0]], buffer, offset - 21, 0);
  _overlay(font_std_fixed_ascii[(unsigned)chars[1]], buffer, offset - 16, 0);
  _overlay(font_std_fixed_ascii[(unsigned)chars[2]], buffer, offset - 11, 0);
  _overlay(font_std_fixed_ascii[(unsigned)chars[3]], buffer, offset -  6, 0);
  //_overlay(font_std_fixed_ascii[(unsigned)chars[4]], buffer, offset);

  return SUCCESS;
}

// clean up after finishing with the device
void ldisplay_cleanup() {
	if (!udev)
    return; // nothing to do

  die_anim_thread = 1;
  pthread_join(anim_thread, NULL);

	// release the interface
	usb_release_interface(udev,0);

	// close the device
  usb_close(udev);
}

void ldisplay_dumpBuffer(uint32_t data[7]) {
  int i, j;

  printf("\n");
  for (i=0; i<7; ++i) {
    for (j=21; j>=0; --j) {
      printf( ((data[i] >> j) & 0x1) ? "#" : "-" );
    }
    printf("\n");
  }
  printf("\n");
}
