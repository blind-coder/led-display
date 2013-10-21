/*
 * Copyright (C) 2006 David Ingram
 * Copyright (C) 2013 Benjamin Schieder - implementing Conways Game of Life
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <stdbool.h>
#include "binconst.h"
#include "libleddisplay.h"
#include "led_font_time.h"

static void sighandler(int sig) {
	// shut down cleanly
	printf("Cleaning up...\n");
	ldisplay_reset();
	ldisplay_cleanup();
	exit(EXIT_SUCCESS);
}

static void usage(char *progname) {
	printf("Usage:\n");
	printf("  --help                                           Display this screen.\n");
	printf("\n");
	exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
	// parse arguments
	int ret;
	{
		struct option long_options[] = {
			{ "help", 0, 0, 0},
			{ 0, 0, 0, 0 }
		};

		while (1) {
			int opt_index=0;
			int c = getopt_long(argc, argv, "", long_options, &opt_index);

			if (c == -1)
				break;

			switch (c) {
				case 0:
					if (opt_index==2) {
						usage(argv[0]);
					}
					break;
				case '?':
					exit(EXIT_FAILURE);
				default:
					printf("??? getopt returned character code 0x%x ???\n", c);
			}
		}
	}

	// no output buffering please
	setbuf(stdout,0);

	printf("LED Display: clock program with game-of-life simulation\n");

	// initialise device
	if (ldisplay_init() != SUCCESS) {
		fprintf(stderr, "\033[1;31mDevice failed to initialise!\033[0m\n");
		return 1;
	}

	struct sigaction sigact = {
		.sa_handler = sighandler
	};

	// prepare for signals
	sigaction(SIGHUP, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	// reset it to a known initial state
	if ((ret = ldisplay_reset())) {
		fprintf(stderr, "\033[1;31mDevice failed to reset: %d\033[0m\n", ret);
		ldisplay_cleanup();
		return 1;
	}

	ldisplay_setBrightness(LDISPLAY_DIM);
	int oldtime_int=0;
	time_t oldtime_t=0;

	uint32_t buffer[7] = {0};

	bool matrix[21][7];
	while (1) {
		time_t t = time(NULL);
		struct tm *curTime = localtime(&t);
		int time = (100*curTime->tm_hour) + curTime->tm_min;
		int i;
		uint32_t j;

		if (oldtime_int != time){
			/* Every minute, on the minute, set the time to the display */
			oldtime_t = t+1;
			oldtime_int = time;
			for (i=0; i<7; i++){
				buffer[i] = B24(00000, 00000000, 00000000);
			}
			_overlay(time_font_colon, buffer, 0, 0);
			_overlay(time_segment_font_digits[(time     )%10], buffer,   0, 0);
			_overlay(time_segment_font_digits[(time/10  )%10], buffer, - 5, 0);
			_overlay(time_segment_font_digits[(time/100 )%10], buffer, -12, 0);
			_overlay(time_segment_font_digits[(time/1000)%10], buffer, -17, 0);
		} else
		if ((buffer[0] | buffer[1] | buffer[2] | buffer[3] | buffer[4] | buffer[5] | buffer[6]) == 0){
			/* oops, display is empty. Add a lonely glider */
			buffer[0] = B24(00000, 00000000, 00000000);
			buffer[1] = B24(00000, 00000000, 00000000);
			buffer[2] = B24(00000, 00010000, 00000000);
			buffer[3] = B24(00000, 00001000, 00000000);
			buffer[4] = B24(00000, 00111000, 00000000);
			buffer[5] = B24(00000, 00000000, 00000000);
			buffer[6] = B24(00000, 00000000, 00000000);
		}
		ldisplay_setDisplay(buffer);

		/*
		printf("[H[J");
		for(i=0; i<7; i++){
			uint32_t val;
			j=0;
			val=buffer[i];
			while (val > 0){
				if (val & 1){
					printf("O");
					matrix[j][i] = true;
				} else {
					printf(" ");
					matrix[j][i] = false;
				}
				j++;
				val = val >> 1;
			}
			printf("\n");
		}
		*/

		usleep(100000);
		if (oldtime_t < t){
			oldtime_t = t;
		// Conways game of life
		bool newmatrix[21][7];
		for (i = 0; i<7; i++){
			for (j=0; j<21; j++){
				newmatrix[j][i] = matrix[j][i];
			}
		}

		for (i = 0; i<7; i++){
			buffer[i] = B24(00000, 00000000, 00000000);
			for (j=0; j<21; j++){
				int neighbors = 0;
				int l, r, t, b;
				if (i == 0){ t = 6; }
				else { t = i - 1; }
				if (i == 6){ b = 0; }
				else { b = i + 1; }
				if (j == 0){ l = 20; }
				else { l = j - 1; }
				if (j == 20){ r = 0; }
				else { r = j + 1; }
				if (matrix[l][t]) neighbors++;
				if (matrix[l][i]) neighbors++;
				if (matrix[l][b]) neighbors++;
				if (matrix[j][t]) neighbors++;
				//if (matrix[j][i]) neighbors++; // not counting ourself
				if (matrix[j][b]) neighbors++;
				if (matrix[r][t]) neighbors++;
				if (matrix[r][i]) neighbors++;
				if (matrix[r][b]) neighbors++;
				if (!(matrix[j][i])){
					if (neighbors == 3){
						newmatrix[j][i] = true;
					} else {
						newmatrix[j][i] = false;
					}
				} else {
					if (neighbors < 2){
						newmatrix[j][i] = false;
					} else if (neighbors <= 3){
						newmatrix[j][i] = true;
					} else {
						newmatrix[j][i] = false;
					}
				}
				// different ruleset
				/*
				if (neighbors % 2 == 0){
					matrix[i][j] = false;
				} else {
					matrix[i][j] = true;
				}
				*/
				buffer[i] |= (newmatrix[j][i] ? 1 << j : 0);
			}
		}
		for (i = 0; i<7; i++){
			for (j=0; j<21; j++){
				matrix[j][i] = newmatrix[j][i];
			}
		}
		}
	}

	ldisplay_reset();
	ldisplay_cleanup();
	return 0;
}
