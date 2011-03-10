/****************************************************************************
 * Slampler : Slug Sampler
 *
 * Stage-oriented sample player
 *
 * Copyright (C) Jean Zundel <jzu@free.fr> 2010 
 *
 * The Slampler is a sample player designed for the Linksys NSLU2 running 
 * GNU/Linux, but it works on any ALSA-based architecture.
 *
 *  gcc slampler.c -Wall -g -lasound -lpthread -o slampler
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 ****************************************************************************/

#define ALSA_PCM_NEW_HW_PARAMS_API

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <dirent.h>

#define DATADIR "/data"
#define NBANKS 3
#define NSMPLS 5

#define INITIAL_LENGTH 0x100000

#define DEBUG if (debug) printf
#define ERROR if (debug) fprintf

// Switches 6B745  547B6

#define SW_SMPL0 9
#define SW_SMPL1 7
#define SW_SMPL2 4
#define SW_SMPL3 5
#define SW_SMPL4 8
#define SW_BANK  6
#define SW_SMPL5 0
#define SW_SMPL6 2
#define SW_SMPL7 3
#define SW_SMPL8 1

/*
#define SW_SMPL0 0
#define SW_SMPL1 1
#define SW_SMPL2 2
#define SW_SMPL3 3
#define SW_SMPL4 4
#define SW_SMPL5 5
#define SW_SMPL6 6
#define SW_SMPL7 7
#define SW_SMPL8 8
#define SW_BANK  9
*/

// LEDs

#define LED_DISK1  "/sys/class/leds/nslu2:green:disk-1"
#define LED_DISK2  "/sys/class/leds/nslu2:green:disk-2"
#define LED_READY  "/sys/class/leds/nslu2:green:ready"
#define LED_STATUS "/sys/class/leds/nslu2:red:status"

// WAV handling

struct RIFFfmtdata {
  char  data1 [22];
  short numchannels;
  char  data2 [16];
  int   size;
};
struct RIFFfmtdata wavehead [NBANKS][NSMPLS];

short *wave [NBANKS][NSMPLS],
      *smpl_ptr [NSMPLS];

int   smpl_flag [NSMPLS];

int bank = 0;

pthread_t thread;                    // Joystick thread

snd_pcm_uframes_t frames = 44;       // But will be set later
snd_pcm_t *handle_play;

short *playbuf;

int   debug = 0;


void  *joystick ();
void  set_led (char *led, int i);
void  load_waves (int rep);
void  debugsig (int signum);


/****************************************************************************
 * Init, main read/process/write loop
 ****************************************************************************/ 

int main (int argc, char **argv) {

  int s,
      i;
  int rc;                            // Result for ALSA operations
  unsigned int rate = 44100;         // Sample rate

//  snd_pcm_hw_params_t *params_play;


  if ((argc > 1) && (!strcmp(argv[1], "-d")))
    debug = 1;

  /* LEDs off */

  set_led (LED_DISK1, 0);
  set_led (LED_DISK2, 0);
  set_led (LED_READY, 0);
  set_led (LED_STATUS,0);

  memset (smpl_ptr, 0, sizeof (short *) * NSMPLS);

  for (i = 0; i < NBANKS; i++)
    load_waves (i);

  /* Joystick thread */

  pthread_create (&thread, NULL, joystick, NULL);

  signal (SIGINT, debugsig);

  // ALSA init

  playbuf = (short *) malloc (frames * 4);

  /* PCM playback setup - stereo : some soundcards don't do mono */

  rc = snd_pcm_open (&handle_play, "plughw:1", SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    ERROR (stderr,
           "play - unable to open pcm device: %s\n", snd_strerror (rc));
    exit (1);
  }
  if ((rc = snd_pcm_set_params (handle_play,
                                SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                2,                               // Stereo
                                rate,
                                1,                               // Resample
                                80000)) < 0) {                   // 0.08 sec 
    ERROR (stderr,
           "Playback open error: %s\n", snd_strerror (rc));
    exit (EXIT_FAILURE);
  }

  /* Processing loop */

  while (1) {

    /* Has a new sample been activated ? */

    for (s = 0; s < NSMPLS; s++)
      if (smpl_flag [s]) {
        smpl_flag [s] = 0;
        smpl_ptr [s] = wave [bank][s];
        DEBUG ("start\n");
      }

    /* Mix samples in playback buffer */

    memset (playbuf, 0, frames*4);                     // Stereo, 16-bit
                                              
    for (s = 0; s < NSMPLS; s++)
      if (smpl_ptr [s]) {
        for (i = 0; i < frames*2; i += 2) {
          if ((playbuf [i] > 0) &&
              (smpl_ptr [s][i] > 0) &&
              (playbuf [i] + smpl_ptr [s][i] < 0))
            playbuf [i] = playbuf [i+1] = SHRT_MAX;    // Prevents rollovers
          else 
          if ((playbuf [i] < 0) &&
              (smpl_ptr [s][i] < 0) &&
              (playbuf [i] + smpl_ptr [s][i] > 0))
            playbuf [i] = playbuf [i+1] = SHRT_MIN;
          else {
            playbuf [i]   += smpl_ptr [s][i]/2;        // Mono to stereo
            playbuf [i+1] += smpl_ptr [s][i]/2;
          }
        }
        smpl_ptr [s] += (frames * 2);
        if (smpl_ptr [s] >= wave [bank][s] + wavehead [bank][s].size / 2) {
          smpl_ptr [s] = 0;                            // Hoc finiunt samples
          DEBUG ("stop\n");
        }
      }

    /* Write playback buffer content to device */

    rc = snd_pcm_writei (handle_play, playbuf, frames);

    if (rc == -EPIPE) {
      ERROR (stderr, 
             "writei - underrun occurred\n");
      snd_pcm_prepare (handle_play);
      snd_pcm_writei (handle_play, playbuf, frames);
    } else if (rc < 0) {
      ERROR (stderr,
             "error from write: %s\n", snd_strerror (rc));
    }  else if (rc != (int)frames) {
      ERROR (stderr,
             "short write, write %d frames\n", rc);
    }
  }

  return 0;
}


/**************************************************************************** 
 * joystick()
 *
 * Separate thread
 * Access to four switches, switches NSLU2 LEDs on/off
 * Could use another joystick for 4 more switches (open ("/dev/input/js1...)
 ****************************************************************************/

void *joystick ()
{
  int jfd;
  int s;
  int i;
  short bringback;      // Dummy short to bring back samples from swap
  struct js_event ev,
                  oldev;

  memset (&oldev, 0, sizeof (struct js_event));
  memset (smpl_flag, 0, sizeof (int) * NSMPLS);

  jfd = open ("/dev/input/js0", O_RDONLY);

  while (1) {
    if (read (jfd, &ev, sizeof (ev)) > 0) {
      if ((ev.type == JS_EVENT_BUTTON) &&
               (ev.value == 1)) {
        if      (ev.number == SW_SMPL0) {          // Yeuch
          smpl_flag[0] ^= 1;
          DEBUG ("smpl0=%d\n", smpl_flag[0]);
        }
        else if (ev.number == SW_SMPL1) {
          smpl_flag[1] ^= 1;
          DEBUG ("smpl1=%d\n", smpl_flag[1]);
        }
        else if (ev.number == SW_SMPL2) {
          smpl_flag[2] ^= 1;
          DEBUG ("smpl2=%d\n", smpl_flag[2]);
        }
        else if (ev.number == SW_SMPL3) {
          smpl_flag[3] ^= 1;
          DEBUG ("smpl3=%d\n", smpl_flag[3]);
        }
        else if (ev.number == SW_SMPL4) {
          smpl_flag[4] ^= 1;
          DEBUG ("smpl4=%d\n", smpl_flag[4]);
        }
        else if (ev.number == SW_SMPL5) {
          smpl_flag[5] ^= 1;
          DEBUG ("smpl5=%d\n", smpl_flag[5]);
        }
        else if (ev.number == SW_SMPL6) {
          smpl_flag[6] ^= 1;
          DEBUG ("smpl6=%d\n", smpl_flag[6]);
        }
        else if (ev.number == SW_SMPL7) {
          smpl_flag[7] ^= 1;
          DEBUG ("smpl7=%d\n", smpl_flag[7]);
        }
        else if (ev.number == SW_SMPL8) {
          smpl_flag[8] ^= 1;
          DEBUG ("smpl8=%d\n", smpl_flag[8]);
        }
        else if (ev.number == SW_BANK) {
          for (s = 0; s < NSMPLS; s++) {          // Shut off running samples
            smpl_ptr [s] = 0;
          }
          for (s = 0; s < NSMPLS; s++)            // Bring samples from swap
            for (i = 0; i < wavehead [bank][s].size / 2; i++)
              bringback = wave [bank][s][i];
          switch (++bank) {
            case 3:
              bank = 0;
            case 0:
              set_led (LED_DISK1, 255);
              set_led (LED_DISK2,   0);
              set_led (LED_READY,   0);
              break;
            case 1:
              set_led (LED_DISK1,   0);
              set_led (LED_DISK2, 255);
              set_led (LED_READY,   0);
              break;
            case 2:
              set_led (LED_DISK1,   0);
              set_led (LED_DISK2,   0);
              set_led (LED_READY, 255);
              break;
            default:
              bank = 0;
              break;
          }
          DEBUG ("bank %d\n", bank);
        }
        else {
          DEBUG ("ev.number=%d\n", ev.number);
        }
        if ((ev.value == 1) && 
            (oldev.value == 1) && 
            (((ev.number == SW_SMPL4) && (oldev.number == SW_BANK)) || 
             ((ev.number == SW_BANK)  && (oldev.number == SW_SMPL4)))) {
          set_led (LED_READY, 0);
          set_led (LED_DISK1, 0);
          set_led (LED_DISK2, 0);
          set_led (LED_STATUS,1);
          exit (0);               // Bye if both Bank and Sample 4 are pressed
        }
      }
    }
    oldev.value  = ev.value; 
    oldev.number = ev.number;
  }
}


/**************************************************************************** 
 * write_to_file()
 *
 * Write a string to a file 
 * Fails silently (in case of insufficient rights)
 * *f  Filename
 * *s  String to write
 ****************************************************************************/

void write_to_file (const char *f, const char *s) {

  int fout;

  fout = open (f, O_WRONLY);
  write (fout, s, strlen (s));
  close (fout);
}

/****************************************************************************
 * set_led()
 *
 * Switch LEDs on/off 
 * Needs to be run as root to work, else nothing happens
 * *led  Dirname (/sys/classes/leds/foo/)
 * i     Value   (0||!0)
 ****************************************************************************/

void set_led (char *led, int i) {

  char led_bright [256];

  strncpy (led_bright, led, 255);
  strncat (led_bright, "/brightness", 255);
  write_to_file (led_bright, i ? "255" : "0");
}   


/****************************************************************************
 * load_waves()
 *
 * Loads .WAVs from a (numerically named) directory for a sample bank
 * All files at 44100 Hz, 2 bytes/sample, mono or stereo, normalized at -3dB
 *
 * rep  Number for directory name (should be 0,1,2...)
 ****************************************************************************/

void load_waves (int rep) {

  DIR *dirp;
  struct dirent *dp;
  int f, 
      i;
  int wfile;
  char repname [256], 
       wname [256];

  sprintf (repname, "%s/%d", DATADIR, rep);
  if ((dirp = opendir (repname)) != NULL) {
    f = 0;
    while (((dp = readdir (dirp)) != NULL) && (f < NSMPLS)) {
      if (dp->d_name[0] != '.') {
        sprintf (wname, "%s/%d/%s", DATADIR, rep, dp->d_name);
        wfile = open (wname, O_RDONLY);
        read (wfile, &wavehead [rep][f], sizeof (struct RIFFfmtdata));
        wave [rep][f] = malloc (wavehead [rep][f].size);
        read (wfile, wave [rep][f], wavehead [rep][f].size);
        if (wavehead [rep][f].numchannels == 2)                // Back to mono
          for (i = 0; i < wavehead [rep][f].size / 2 ; i += 2)
            wave [rep][f][i] = wave [rep][f][i] / 2 + wave [rep][f][i+1] / 2;
        DEBUG ("%10d  %s\n", wavehead [rep][f].size, wname);
        close (wfile);
        f++;
      }
    }
    closedir (dirp);
    return;
  }
  else {
    ERROR (stderr,
           "Caramba, totale felure sur %s\n", repname);
  }
}



/****************************************************************************
 * debugsig()
 *
 * Signal handler
 * signum  Signal number
 ****************************************************************************/

void debugsig (int signum) {

  exit(0);
}

