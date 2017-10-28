#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <flash_utils.h>
extern "C" {
  #include <spi_flash.h>
}
#pragma GCC optimize ("O3")

#include <AudioOutputNull.h>
#include <AudioOutputI2SDAC.h>
//#include <AudioOutputSPIFFSWAV.h>
#include <ESP8266FastROMFS.h>
#include <AudioFileSourceFastROMFS.h>

#define TSF_NO_STDIO
#define TSF_IMPLEMENTATION
#include "tsf.h"

#include "AudioGenerator.h"

class AudioGeneratorMIDI : AudioGenerator
{
  public:
    AudioGeneratorMIDI() { freq=44100; };
    virtual ~AudioGeneratorMIDI() override {};
    bool SetSoundfont(AudioFileSource *newsf2) {
      if (isRunning()) return false;
      sf2 = newsf2;
      MakeStreamFromAFS(sf2, &afsSF2);
      return true;
    }
    bool SetSampleRate(int newfreq) {
      if (isRunning()) return false;
      freq = newfreq;
      return true;
    }
    virtual bool begin(AudioFileSource *mid, AudioOutput *output) override;
    virtual bool loop() override;
    virtual bool stop() override;
    virtual bool isRunning() override { return running; };

  private:
    int freq;
    tsf *g_tsf;
    struct tsf_stream buffer;
    struct tsf_stream afsMIDI;
    struct tsf_stream afsSF2;
    AudioFileSource *sf2;
    AudioFileSource *midi;

  protected:
    struct midi_header {
      int8_t MThd[4];
      uint32_t header_size;
      uint16_t format_type;
      uint16_t number_of_tracks;
      uint16_t time_division;
    };

    struct track_header {
      int8_t MTrk[4];
      uint32_t track_size;
    };




    enum { MAX_TONEGENS = 32,         /* max tone generators: tones we can play simultaneously */
           MAX_TRACKS = 24
         };         /* max number of MIDI tracks we will process */

    int hdrptr;
    unsigned long buflen;
    int num_tracks;
    int tracks_done = 0;
    int num_tonegens = MAX_TONEGENS;
    int num_tonegens_used = 0;
    unsigned int ticks_per_beat = 240;
    unsigned long timenow = 0;
    unsigned long tempo;            /* current tempo in usec/qnote */
    // State needed for PlayMID()
    int notes_skipped = 0;
    int tracknum = 0;
    int earliest_tracknum = 0;
    unsigned long earliest_time = 0;

    struct tonegen_status {         /* current status of a tone generator */
      bool playing;                /* is it playing? */
      char track;                   /* if so, which track is the note from? */
      char note;                    /* what note is playing? */
      char instrument;              /* what instrument? */
    } tonegen[MAX_TONEGENS] = {
      {
        0
      }
    };

    struct track_status {           /* current processing point of a MIDI track */
      int trkptr;                  /* ptr to the next note change */
      int trkend;                  /* ptr past the end of the track */
      unsigned long time;          /* what time we're at in the score */
      unsigned long tempo;         /* the tempo last set, in usec per qnote */
      unsigned int preferred_tonegen;      /* for strategy2, try to use this generator */
      unsigned char cmd;           /* CMD_xxxx next to do */
      unsigned char note;          /* for which note */
      unsigned char chan;          /* from which channel it was */
      unsigned char velocity;      /* the current volume */
      unsigned char last_event;    /* the last event, for MIDI's "running status" */
      bool tonegens[MAX_TONEGENS]; /* which tone generators our notes are playing on */
    } track[MAX_TRACKS] = {
      {
        0
      }
    };

    int midi_chan_instrument[16] = {
      0
    };                              /* which instrument is currently being played on each channel */

    /* output bytestream commands, which are also stored in track_status.cmd */
    enum { CMD_PLAYNOTE   = 0x90,    /* play a note: low nibble is generator #, note is next byte */
           CMD_STOPNOTE   = 0x80,    /* stop a note: low nibble is generator # */
           CMD_INSTRUMENT = 0xc0,    /* change instrument; low nibble is generator #, instrument is next byte */
           CMD_RESTART    = 0xe0,    /* restart the score from the beginning */
           CMD_STOP       = 0xf0,    /* stop playing */
           CMD_TEMPO      = 0xFE,    /* tempo in usec per quarter note ("beat") */
           CMD_TRACKDONE  = 0xFF
         };   /* no more data left in this track */



    /* portable string length */
    int strlength (const char *str) {
      int i;
      for (i = 0; str[i] != '\0'; ++i);
      return i;
    }


    /* match a constant character sequence */

    int charcmp (const char *buf, const char *match) {
      int len, i;
      len = strlength (match);
      for (i = 0; i < len; ++i)
        if (buf[i] != match[i])
          return 0;
      return 1;
    }

    unsigned char buffer_byte (int offset);
    unsigned short buffer_short (int offset);
    unsigned int buffer_int32 (int offset);

    void midi_error (char *msg, int curpos);
    void chk_bufdata (int ptr, unsigned long int len);
    uint16_t rev_short (uint16_t val);
    uint32_t rev_long (uint32_t val);
    void process_header (void);
    void start_track (int tracknum);

    unsigned long get_varlen (int *ptr);
    void find_note (int tracknum);
    void PrepareMIDI(AudioFileSource *src);
    int PlayMIDI();
    void StopMIDI();

    // tsf_stream <-> AudioFileSource
    static int afs_read(void *data, void *ptr, unsigned int size);
    static int afs_tell(void *data);
    static int afs_skip(void *data, unsigned int count);
    static int afs_seek(void *data, unsigned int pos);
    static int afs_close(void *data);
    static int afs_size(void *data);
    void MakeStreamFromAFS(AudioFileSource *src, tsf_stream *afs);

    int samplesToPlay;
    bool sawEOF;
    int numSamplesRendered;
    int sentSamplesRendered ;
    short samplesRendered[256];
};



/*********************************************************************************************

   MIDITONES: Convert a MIDI file into a simple bytestream of notes

  ----------------------------------------------------------------------------------------
  The MIT License (MIT)
  Copyright (c) 2011,2013,2015,2016, Len Shustek

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR
  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*********************************************************************************************/


/***********  MIDI file header formats  *****************/

/****************  utility routines  **********************/

/* announce a fatal MIDI file format error */

void AudioGeneratorMIDI::midi_error(char *msg, int curpos)
{
  int ptr;
  Serial.printf("---> MIDI file error at position %04X (%d): %s\n", (uint16_t) curpos, (uint16_t) curpos, msg);
  /* print some bytes surrounding the error */
  ptr = curpos - 16;
  if (ptr < 0) ptr = 0;
  buffer.seek( buffer.data, ptr );
  for (int i = 0; i < 32; i++) {
    char c;
    buffer.read (buffer.data, &c, 1);
    Serial.printf((ptr + i) == curpos ? " [%02X]  " : "%02X ", (int) c & 0xff);
  }
  Serial.printf("\n");
  running = false;
}

/* check that we have a specified number of bytes left in the buffer */

void AudioGeneratorMIDI::chk_bufdata (int ptr, unsigned long int len) {
  if ((unsigned) (ptr + len) > buflen)
    midi_error ("data missing", ptr);
}

/* fetch big-endian numbers */

uint16_t AudioGeneratorMIDI::rev_short (uint16_t val) {
  return ((val & 0xff) << 8) | ((val >> 8) & 0xff);
}

uint32_t AudioGeneratorMIDI::rev_long (uint32_t val) {
  return (((rev_short ((uint16_t) val) & 0xffff) << 16) |
          (rev_short ((uint16_t) (val >> 16)) & 0xffff));
}

/**************  process the MIDI file header  *****************/

void AudioGeneratorMIDI::process_header (void) {
  struct midi_header hdr;
  unsigned int time_division;

  chk_bufdata (hdrptr, sizeof (struct midi_header));
  buffer.seek (buffer.data, hdrptr);
  buffer.read (buffer.data, &hdr, sizeof (hdr));
  if (!charcmp ((char *) hdr.MThd, "MThd"))
    midi_error ("Missing 'MThd'", hdrptr);
  num_tracks = rev_short (hdr.number_of_tracks);
  time_division = rev_short (hdr.time_division);
  if (time_division < 0x8000)
    ticks_per_beat = time_division;
  else
    ticks_per_beat = ((time_division >> 8) & 0x7f) /* SMTE frames/sec */ *(time_division & 0xff);     /* ticks/SMTE frame */
  hdrptr += rev_long (hdr.header_size) + 8;    /* point past header to track header, presumably. */
  return;
}


/****************  Process a MIDI track header *******************/

void AudioGeneratorMIDI::start_track (int tracknum) {
  struct track_header hdr;
  unsigned long tracklen;

  chk_bufdata (hdrptr, sizeof (struct track_header));
  buffer.seek (buffer.data, hdrptr);
  buffer.read (buffer.data, &hdr, sizeof (hdr));
  if (!charcmp ((char *) (hdr.MTrk), "MTrk"))
    midi_error ("Missing 'MTrk'", hdrptr);
  tracklen = rev_long (hdr.track_size);
  hdrptr += sizeof (struct track_header);      /* point past header */
  chk_bufdata (hdrptr, tracklen);
  track[tracknum].trkptr = hdrptr;
  hdrptr += tracklen;          /* point to the start of the next track */
  track[tracknum].trkend = hdrptr;     /* the point past the end of the track */
}

unsigned char AudioGeneratorMIDI::buffer_byte (int offset) {
  unsigned char c;
  buffer.seek (buffer.data, offset);
  buffer.read (buffer.data, &c, 1);
  return c;
}

unsigned short AudioGeneratorMIDI::buffer_short (int offset) {
  unsigned short s;
  buffer.seek (buffer.data, offset);
  buffer.read (buffer.data, &s, sizeof (short));
  return s;
}

unsigned int AudioGeneratorMIDI::buffer_int32 (int offset) {
  uint32_t i;
  buffer.seek (buffer.data, offset);
  buffer.read (buffer.data, &i, sizeof (i));
  return i;
}

/* Get a MIDI-style variable-length integer */

unsigned long AudioGeneratorMIDI::get_varlen (int *ptr) {
  /* Get a 1-4 byte variable-length value and adjust the pointer past it.
    These are a succession of 7-bit values with a MSB bit of zero marking the end */

  unsigned long val;
  int i, byte;

  val = 0;
  for (i = 0; i < 4; ++i) {
    byte = buffer_byte ((*ptr)++);
    val = (val << 7) | (byte & 0x7f);
    if (!(byte & 0x80))
      return val;
  }
  return val;
}


/***************  Process the MIDI track data  ***************************/

/* Skip in the track for the next "note on", "note off" or "set tempo" command,
  then record that information in the track status block and return. */

void AudioGeneratorMIDI::find_note (int tracknum) {
  unsigned long int delta_time;
  int event, chan;
  int note, velocity, controller, pressure, pitchbend, instrument;
  int meta_cmd, meta_length;
  unsigned long int sysex_length;
  struct track_status *t;
  char *tag;

  /* process events */

  t = &track[tracknum];        /* our track status structure */
  while (t->trkptr < t->trkend) {

    delta_time = get_varlen (&t->trkptr);
    t->time += delta_time;
    if (buffer_byte (t->trkptr) < 0x80)
      event = t->last_event; /* using "running status": same event as before */
    else {                    /* otherwise get new "status" (event type) */
      event = buffer_byte (t->trkptr++);
    }
    if (event == 0xff) {      /* meta-event */
      meta_cmd = buffer_byte (t->trkptr++);
      meta_length = buffer_byte (t->trkptr++);
      switch (meta_cmd) {
        case 0x00:
          break;
        case 0x01:
          tag = "description";
          goto show_text;
        case 0x02:
          tag = "copyright";
          goto show_text;
        case 0x03:
          tag = "track name";
          goto show_text;
        case 0x04:
          tag = "instrument name";
          goto show_text;
        case 0x05:
          tag = "lyric";
          goto show_text;
        case 0x06:
          tag = "marked point";
          goto show_text;
        case 0x07:
          tag = "cue point";
show_text:
          break;
        case 0x20:
          break;
        case 0x2f:
          break;
        case 0x51:            /* tempo: 3 byte big-endian integer! */
          t->cmd = CMD_TEMPO;
          t->tempo = rev_long (buffer_int32 (t->trkptr - 1)) & 0xffffffL;
          t->trkptr += meta_length;
          return;
        case 0x54:
          break;
        case 0x58:
          break;
        case 0x59:
          break;
        case 0x7f:
          tag = "sequencer data";
          goto show_hex;
        default:              /* unknown meta command */
          tag = "???";
show_hex:
          break;
      }
      t->trkptr += meta_length;
    }

    else if (event < 0x80)
      midi_error ("Unknown MIDI event type", t->trkptr);

    else {
      if (event < 0xf0)
        t->last_event = event;      // remember "running status" if not meta or sysex event
      chan = event & 0xf;
      t->chan = chan;
      switch (event >> 4) {
        case 0x8:
          t->note = buffer_byte (t->trkptr++);
          velocity = buffer_byte (t->trkptr++);
note_off:
          t->cmd = CMD_STOPNOTE;
          return;             /* stop processing and return */
        case 0x9:
          t->note = buffer_byte (t->trkptr++);
          velocity = buffer_byte (t->trkptr++);
          if (velocity == 0)  /* some scores use note-on with zero velocity for off! */
            goto note_off;
          t->velocity = velocity;
          t->cmd = CMD_PLAYNOTE;
          return;             /* stop processing and return */
        case 0xa:
          note = buffer_byte (t->trkptr++);
          velocity = buffer_byte (t->trkptr++);
          break;
        case 0xb:
          controller = buffer_byte (t->trkptr++);
          velocity = buffer_byte (t->trkptr++);
          break;
        case 0xc:
          instrument = buffer_byte (t->trkptr++);
          midi_chan_instrument[chan] = instrument;    // record new instrument for this channel
          break;
        case 0xd:
          pressure = buffer_byte (t->trkptr++);
          break;
        case 0xe:
          pitchbend = buffer_byte (t->trkptr) | (buffer_byte (t->trkptr + 1) << 7);
          t->trkptr += 2;
          break;
        case 0xf:
          sysex_length = get_varlen (&t->trkptr);
          t->trkptr += sysex_length;
          break;
        default:
          midi_error ("Unknown MIDI command", t->trkptr);
      }
    }
  }
  t->cmd = CMD_TRACKDONE;      /* no more notes to process */
  ++tracks_done;
}


// Open file, parse headers, get ready tio process MIDI
void AudioGeneratorMIDI::PrepareMIDI(AudioFileSource *src)
{
  MakeStreamFromAFS(src, &afsMIDI);
  tsf_stream_wrap_cached(&afsMIDI, 32, 64, &buffer);
  buflen = buffer.size (buffer.data);

  /* process the MIDI file header */

  hdrptr = buffer.tell (buffer.data);  /* pointer to file and track headers */
  process_header ();
  printf ("  Processing %d tracks.\n", num_tracks);
  if (num_tracks > MAX_TRACKS)
    midi_error ("Too many tracks", buffer.tell (buffer.data));

  /* initialize processing of all the tracks */

  for (tracknum = 0; tracknum < num_tracks; ++tracknum) {
    start_track (tracknum);   /* process the track header */
    find_note (tracknum);     /* position to the first note on/off */
  }

  notes_skipped = 0;
  tracknum = 0;
  earliest_tracknum = 0;
  earliest_time = 0;
}

// Parses the note on/offs ujntil we are ready to render some more samples.  Then return the
// total number of samples to render before we need to be called again
int AudioGeneratorMIDI::PlayMIDI()
{
  /* Continue processing all tracks, in an order based on the simulated time.
    This is not unlike multiway merging used for tape sorting algoritms in the 50's! */

  do {                         /* while there are still track notes to process */
    static struct track_status *trk;
    static struct tonegen_status *tg;
    static int tgnum;
    static int count_tracks;
    static unsigned long delta_time, delta_msec;

    /* Find the track with the earliest event time,
       and output a delay command if time has advanced.

       A potential improvement: If there are multiple tracks with the same time,
       first do the ones with STOPNOTE as the next command, if any.  That would
       help avoid running out of tone generators.  In practice, though, most MIDI
       files do all the STOPNOTEs first anyway, so it won't have much effect.
    */

    earliest_time = 0x7fffffff;

    /* Usually we start with the track after the one we did last time (tracknum),
       so that if we run out of tone generators, we have been fair to all the tracks.
       The alternate "strategy1" says we always start with track 0, which means
       that we favor early tracks over later ones when there aren't enough tone generators.
    */

    count_tracks = num_tracks;
    do {
      if (++tracknum >= num_tracks)
        tracknum = 0;
      trk = &track[tracknum];
      if (trk->cmd != CMD_TRACKDONE && trk->time < earliest_time) {
        earliest_time = trk->time;
        earliest_tracknum = tracknum;
      }
    } while (--count_tracks);

    tracknum = earliest_tracknum;     /* the track we picked */
    trk = &track[tracknum];
    if (earliest_time < timenow)
      midi_error ("INTERNAL: time went backwards", trk->trkptr);

    /* If time has advanced, output a "delay" command */

    delta_time = earliest_time - timenow;
    if (delta_time) {
      /* Convert ticks to milliseconds based on the current tempo */
      unsigned long long temp;
      temp = ((unsigned long long) delta_time * tempo) / ticks_per_beat;
      delta_msec = temp / 1000;      // get around LCC compiler bug
      if (delta_msec > 0x7fff)
        midi_error ("INTERNAL: time delta too big", trk->trkptr);
      int samples = (((int) delta_msec) * freq) / 1000;
      timenow = earliest_time;
      return samples;
    }
    timenow = earliest_time;

    /*  If this track event is "set tempo", just change the global tempo.
       That affects how we generate "delay" commands. */

    if (trk->cmd == CMD_TEMPO) {
      tempo = trk->tempo;
      find_note (tracknum);
    }

    /*  If this track event is "stop note", process it and all subsequent "stop notes" for this track
       that are happening at the same time. Doing so frees up as many tone generators as possible.  */

    else if (trk->cmd == CMD_STOPNOTE)
      do {
        // stop a note
        for (tgnum = 0; tgnum < num_tonegens; ++tgnum) {    /* find which generator is playing it */
          tg = &tonegen[tgnum];
          if (tg->playing && tg->track == tracknum && tg->note == trk->note) {
            tsf_note_off (g_tsf, tg->instrument, tg->note);
            tg->playing = false;
            trk->tonegens[tgnum] = false;
          }
        }
        find_note (tracknum);       // use up the note
      } while (trk->cmd == CMD_STOPNOTE && trk->time == timenow);

    /*  If this track event is "start note", process only it.
       Don't do more than one, so we allow other tracks their chance at grabbing tone generators. */

    else if (trk->cmd == CMD_PLAYNOTE) {
      bool foundgen = false;
      /* if not, then try for any free tone generator */
      if (!foundgen)
        for (tgnum = 0; tgnum < num_tonegens; ++tgnum) {
          tg = &tonegen[tgnum];
          if (!tg->playing) {
            foundgen = true;
            break;
          }
        }
      if (foundgen) {
        if (tgnum + 1 > num_tonegens_used)
          num_tonegens_used = tgnum + 1;
        tg->playing = true;
        tg->track = tracknum;
        tg->note = trk->note;
        trk->tonegens[tgnum] = true;
        trk->preferred_tonegen = tgnum;
        if (tg->instrument != midi_chan_instrument[trk->chan]) {    /* new instrument for this generator */
          tg->instrument = midi_chan_instrument[trk->chan];
        }
        tsf_note_on (g_tsf, tg->instrument, tg->note, trk->velocity / 256.0);
      } else {
        ++notes_skipped;
      }
      find_note (tracknum);     // use up the note
    }
  }
  while (tracks_done < num_tracks);
  return -1; // EOF
}


void AudioGeneratorMIDI::StopMIDI()
{

  buffer.close(buffer.data);
  tsf_close(g_tsf);
  printf ("  %s %d tone generators were used.\n",
          num_tonegens_used < num_tonegens ? "Only" : "All", num_tonegens_used);
  if (notes_skipped)
    printf
    ("  %d notes were skipped because there weren't enough tone generators.\n", notes_skipped);

  printf ("  Done.\n");
}


bool AudioGeneratorMIDI::begin(AudioFileSource *src, AudioOutput *out)
{
  g_tsf = tsf_load(&afsSF2);
  if (!g_tsf) return false;
  tsf_set_output (g_tsf, TSF_MONO, freq, -10 /* dB gain -10 */ );

  if (!out->SetRate( freq )) return false;
  if (!out->SetBitsPerSample( 16 )) return false;
  if (!out->SetChannels( 1 )) return false;
  if (!out->begin()) return false;

  output = out;

  running = true;

  PrepareMIDI(src);

  samplesToPlay = 0;
  numSamplesRendered = 0;
  sentSamplesRendered = 0;
  
  sawEOF = false;
  return running;
}


bool AudioGeneratorMIDI::loop()
{
  static int c = 0;
  if (!running) return true; // Nothing to do here!

  // First, try and push in the stored sample.  If we can't, then punt and try later
  if (!output->ConsumeSample(lastSample)) return true; // Can't send, but no error detected

  // Try and stuff the buffer one sample at a time
  do {
c++;
//if (c==500000) {running = false; return false; }
if (c%44100 == 0) yield();

play:
//Serial.printf("sentSamplesRendered=%d, numSamplesRendered=%d, samplesToPlay=%d, sawEOF=%d\n", sentSamplesRendered, numSamplesRendered, samplesToPlay, sawEOF);
    if (sentSamplesRendered < numSamplesRendered) {
      lastSample[AudioOutput::LEFTCHANNEL] = samplesRendered[sentSamplesRendered];
      lastSample[AudioOutput::RIGHTCHANNEL] = samplesRendered[sentSamplesRendered];
      sentSamplesRendered++;
    } else if (samplesToPlay) {
      numSamplesRendered = sizeof(samplesRendered)/sizeof(samplesRendered[0]);
      if (samplesToPlay < sizeof(samplesRendered)/sizeof(samplesRendered[0])) numSamplesRendered = samplesToPlay;
      tsf_render_short_fast(g_tsf, samplesRendered, numSamplesRendered, 0);
//      tsf_render_short(g_tsf, samplesRendered, numSamplesRendered, 0);
      lastSample[AudioOutput::LEFTCHANNEL] = samplesRendered[0];
      lastSample[AudioOutput::RIGHTCHANNEL] = samplesRendered[0];
      sentSamplesRendered = 1;
      samplesToPlay -= numSamplesRendered;
    } else {
      numSamplesRendered = 0;
      sentSamplesRendered = 0;
      if (sawEOF) {
        running = false;
      } else {
        samplesToPlay = PlayMIDI();
        if (samplesToPlay == -1) {
            sawEOF = true;
            samplesToPlay = freq / 2;
        }
        goto play;
      }
    }
  } while (running && output->ConsumeSample(lastSample));

  return running;
}
 
bool AudioGeneratorMIDI::stop()
{
  return true;
}


int AudioGeneratorMIDI::afs_read(void *data, void *ptr, unsigned int size)
{
  AudioFileSource *s = reinterpret_cast<AudioFileSource *>(data);
  return s->read(ptr, size);
}

int AudioGeneratorMIDI::afs_tell(void *data)
{
  AudioFileSource *s = reinterpret_cast<AudioFileSource *>(data);
  return s->getPos();
}

int AudioGeneratorMIDI::afs_skip(void *data, unsigned int count)
{
  AudioFileSource *s = reinterpret_cast<AudioFileSource *>(data);
  return s->seek(count, SEEK_CUR);
}

int AudioGeneratorMIDI::afs_seek(void *data, unsigned int pos)
{
  AudioFileSource *s = reinterpret_cast<AudioFileSource *>(data);
  return s->seek(pos, SEEK_SET);
}

int AudioGeneratorMIDI::afs_close(void *data)
{
  AudioFileSource *s = reinterpret_cast<AudioFileSource *>(data);
  return s->close();
}

int AudioGeneratorMIDI::afs_size(void *data)
{
  AudioFileSource *s = reinterpret_cast<AudioFileSource *>(data);
  return s->getSize();
}

void AudioGeneratorMIDI::MakeStreamFromAFS(AudioFileSource *src, tsf_stream *afs)
{
  afs->data = reinterpret_cast<void*>(src);
  afs->read = &afs_read;
  afs->tell = &afs_tell;
  afs->skip = &afs_skip;
  afs->seek = &afs_seek;
  afs->close = &afs_close;
  afs->size = &afs_size;
}


AudioFileSourceFastROMFS *sf2;
AudioFileSourceFastROMFS *mid;
//AudioOutputSPIFFSWAV *wav;
AudioOutputI2SDAC *dac;
AudioOutputNull *none;
AudioGeneratorMIDI *midi;
uint32_t t;

FastROMFilesystem *fs;

void setup()
{
  char *soundfont = "1mgm.sf2";
  char *midifile = "furelise.mid";

  WiFi.forceSleepBegin();

  Serial.begin(115200);
  Serial.println("Starting up...\n");

//  SPIFFS.begin();
  fs = new FastROMFilesystem();
  fs->mount();

  sf2 = new AudioFileSourceFastROMFS(fs, soundfont);
  mid = new AudioFileSourceFastROMFS(fs, midifile);
  Serial.printf("sf2=%p\nmid=%p\n", sf2, mid);
  
  none = new AudioOutputNull();
//  wav= new AudioOutputSPIFFSWAV();
  dac = new AudioOutputI2SDAC();
//  wav->SetFilename("/out.wav");
  
  Serial.printf("Creating AudioGeneratorMIDI...\n");
  midi = new AudioGeneratorMIDI();
  Serial.printf("Setting Soundfont...\n");
  midi->SetSoundfont(sf2);
  Serial.printf("Setting Samplerate...\n");
  midi->SetSampleRate(22050);
  Serial.printf("BEGIN...\n");
  midi->begin(mid, dac);
t = millis();
}

void loop()
{
  if (midi->isRunning()) {
    if (!midi->loop()) {
      uint32_t e = millis();
      midi->stop();
//      wav->stop();
      uint32_t d = e - t;
    Serial.printf("Runtime: %dms, %f percent of realtime", d, (500000.0/44100.0) / (d/1000.0) );
    }
    
  } else {
    Serial.printf("MIDI done\n");
    delay(1000);
  }

}


