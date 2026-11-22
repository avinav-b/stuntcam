// SPDX-License-Identifier: Apache-2.0
// Part of simcam.m (single translation unit): the audio source's internal interface. providers/audio.inc includes it
// first. It defines the source's state (gAudio) and declares the functions other fragments call.
//
// Who owns what:
//   The source is started by its first user and stopped by its last, on the pump queue (MicUpdate).
//   The tone's state, and the format while the tone runs, are touched only on gAudio.tone.queue. Its start and stop
//   run there synchronously, so a stop and a start can't free the format under a timer handler that is still running.
//   The microphone sets the format on the pump queue; its callback runs on the microphone's own thread.
//   gAudio.preroll and gAudio.sessions are shared by the source's thread and the pump queue. Each is guarded by
//   @synchronized on itself.
#ifndef SIMCAM_AUDIO_H
#define SIMCAM_AUDIO_H

// The values of gAudio.source.kind.
enum { kAudioSourceTone = 0, kAudioSourceSilence = 1, kAudioSourceMic = 2 };

struct AudioState {
    // The audio source, whichever it is.
    struct {
        int users;                                       // the sessions using it
        int kind;                                        // kAudioSourceTone, kAudioSourceSilence or kAudioSourceMic
        AudioStreamBasicDescription format;              // 16-bit mono PCM (read by providers/pump.inc and
                                                         // client/output_settings.inc)
        CMAudioFormatDescriptionRef formatDescription;   // made anew with each format
    } source;

    // The microphone: its AudioQueue, and its callback's counters.
    struct {
        AudioQueueRef queue;
        long bufferCount;
        double peak;                                     // the peak level since the last level log, 0 to 1
    } mic;

    // The synthetic source (tone or silence).
    struct {
        dispatch_queue_t queue;
        dispatch_source_t timer;
        double phase;
        long bufferCount;
    } tone;

    NSMutableArray *preroll;   // the last ~250 ms of sample buffers, for a recording that starts in the middle of one
    NSHashTable *sessions;     // the running SimcamSessions that want audio
};

// The audio source's state.
static struct AudioState gAudio;

// ---- functions other fragments call

static void MicUpdate(SimcamSession *sess);   // providers/pump.inc, fig/session_hook.inc, core/control.inc
static void MovieAppendVideo(SimcamRecording *recording, CVPixelBufferRef frame, CMTime pts);   // providers/pump.inc

#endif
