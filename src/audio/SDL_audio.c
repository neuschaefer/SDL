/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#include "SDL_sysaudio.h"
#include "../thread/SDL_systhread.h"
#include "../SDL_utils_c.h"

// Available audio drivers
static const AudioBootStrap *const bootstrap[] = {
#ifdef SDL_AUDIO_DRIVER_PULSEAUDIO
    &PULSEAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_ALSA
    &ALSA_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_SNDIO
    &SNDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_NETBSD
    &NETBSDAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_WASAPI
    &WASAPI_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_DSOUND
    &DSOUND_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_HAIKU
    &HAIKUAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_COREAUDIO
    &COREAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_AAUDIO
    &AAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_OPENSLES
    &OPENSLES_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_ANDROID
    &ANDROIDAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_PS2
    &PS2AUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_PSP
    &PSPAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_VITA
    &VITAAUD_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_N3DS
    &N3DSAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_EMSCRIPTEN
    &EMSCRIPTENAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_JACK
    &JACK_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_PIPEWIRE
    &PIPEWIRE_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_OSS
    &DSP_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_QNX
    &QSAAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_DISK
    &DISKAUDIO_bootstrap,
#endif
#ifdef SDL_AUDIO_DRIVER_DUMMY
    &DUMMYAUDIO_bootstrap,
#endif
    NULL
};

static SDL_AudioDriver current_audio;

int SDL_GetNumAudioDrivers(void)
{
    return SDL_arraysize(bootstrap) - 1;
}

const char *SDL_GetAudioDriver(int index)
{
    if (index >= 0 && index < SDL_GetNumAudioDrivers()) {
        return bootstrap[index]->name;
    }
    return NULL;
}

const char *SDL_GetCurrentAudioDriver(void)
{
    return current_audio.name;
}

static int GetDefaultSampleFramesFromFreq(const int freq)
{
    const char *hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    if (hint) {
        const int val = SDL_atoi(hint);
        if (val > 0) {
            return val;
        }
    }

    if (freq <= 22050) {
        return 512;
    } else if (freq <= 48000) {
        return 1024;
    } else if (freq <= 96000) {
        return 2048;
    } else {
        return 4096;
    }
}

void OnAudioStreamCreated(SDL_AudioStream *stream)
{
    SDL_assert(stream != NULL);

    // NOTE that you can create an audio stream without initializing the audio subsystem,
    //  but it will not be automatically destroyed during a later call to SDL_Quit!
    //  You must explicitly destroy it yourself!
    if (current_audio.device_hash_lock) {
        // this isn't really part of the "device list" but it's a convenient lock to use here.
        SDL_LockRWLockForWriting(current_audio.device_hash_lock);
        if (current_audio.existing_streams) {
            current_audio.existing_streams->prev = stream;
        }
        stream->prev = NULL;
        stream->next = current_audio.existing_streams;
        current_audio.existing_streams = stream;
        SDL_UnlockRWLock(current_audio.device_hash_lock);
    }
}

void OnAudioStreamDestroy(SDL_AudioStream *stream)
{
    SDL_assert(stream != NULL);

    // NOTE that you can create an audio stream without initializing the audio subsystem,
    //  but it will not be automatically destroyed during a later call to SDL_Quit!
    //  You must explicitly destroy it yourself!
    if (current_audio.device_hash_lock) {
        // this isn't really part of the "device list" but it's a convenient lock to use here.
        SDL_LockRWLockForWriting(current_audio.device_hash_lock);
        if (stream->prev) {
            stream->prev->next = stream->next;
        }
        if (stream->next) {
            stream->next->prev = stream->prev;
        }
        if (stream == current_audio.existing_streams) {
            current_audio.existing_streams = stream->next;
        }
        SDL_UnlockRWLock(current_audio.device_hash_lock);
    }
}

// device should be locked when calling this.
static SDL_bool AudioDeviceCanUseSimpleCopy(SDL_AudioDevice *device)
{
    SDL_assert(device != NULL);
    return (
        device->logical_devices &&  // there's a logical device
        !device->logical_devices->next &&  // there's only _ONE_ logical device
        !device->logical_devices->postmix && // there isn't a postmix callback
        device->logical_devices->bound_streams &&  // there's a bound stream
        !device->logical_devices->bound_streams->next_binding  // there's only _ONE_ bound stream.
    ) ? SDL_TRUE : SDL_FALSE;
}

// should hold device->lock before calling.
static void UpdateAudioStreamFormatsPhysical(SDL_AudioDevice *device)
{
    if (!device->iscapture) {  // for capture devices, we only want to move to float32 for postmix, which we'll handle elsewhere.
        const SDL_bool simple_copy = AudioDeviceCanUseSimpleCopy(device);
        SDL_AudioSpec spec;

        device->simple_copy = simple_copy;
        SDL_copyp(&spec, &device->spec);

        if (!simple_copy) {
            spec.format = SDL_AUDIO_F32;  // mixing and postbuf operates in float32 format.
        }

        for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
            for (SDL_AudioStream *stream = logdev->bound_streams; stream != NULL; stream = stream->next_binding) {
                // set the proper end of the stream to the device's format.
                // SDL_SetAudioStreamFormat does a ton of validation just to memcpy an audiospec.
                SDL_LockMutex(stream->lock);
                SDL_copyp(&stream->dst_spec, &spec);
                SDL_UnlockMutex(stream->lock);
            }
        }
    }
}


// Zombie device implementation...

// These get used when a device is disconnected or fails, so audiostreams don't overflow with data that isn't being
// consumed and apps relying on audio callbacks don't stop making progress.
static int ZombieWaitDevice(SDL_AudioDevice *device)
{
    if (!SDL_AtomicGet(&device->shutdown)) {
        const int frames = device->buffer_size / SDL_AUDIO_FRAMESIZE(device->spec);
        SDL_Delay((frames * 1000) / device->spec.freq);
    }
    return 0;
}

static int ZombiePlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    return 0;  // no-op, just throw the audio away.
}

static Uint8 *ZombieGetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return device->work_buffer;
}

static int ZombieCaptureFromDevice(SDL_AudioDevice *device, void *buffer, int buflen)
{
    // return a full buffer of silence every time.
    SDL_memset(buffer, device->silence_value, buflen);
    return buflen;
}

static void ZombieFlushCapture(SDL_AudioDevice *device)
{
    // no-op, this is all imaginary.
}



// device management and hotplug...


/* SDL_AudioDevice, in SDL3, represents a piece of physical hardware, whether it is in use or not, so these objects exist as long as
   the system-level device is available.

   Physical devices get destroyed for three reasons:
    - They were lost to the system (a USB cable is kicked out, etc).
    - They failed for some other unlikely reason at the API level (which is _also_ probably a USB cable being kicked out).
    - We are shutting down, so all allocated resources are being freed.

   They are _not_ destroyed because we are done using them (when we "close" a playing device).
*/
static void ClosePhysicalAudioDevice(SDL_AudioDevice *device);


SDL_COMPILE_TIME_ASSERT(check_lowest_audio_default_value, SDL_AUDIO_DEVICE_DEFAULT_CAPTURE < SDL_AUDIO_DEVICE_DEFAULT_OUTPUT);

static SDL_AudioDeviceID AssignAudioDeviceInstanceId(SDL_bool iscapture, SDL_bool islogical)
{
    /* Assign an instance id! Start at 2, in case there are things from the SDL2 era that still think 1 is a special value.
       There's no reasonable scenario where this rolls over, but just in case, we wrap it in a loop.
       Also, make sure we don't assign SDL_AUDIO_DEVICE_DEFAULT_OUTPUT, etc. */

    // The bottom two bits of the instance id tells you if it's an output device (1<<0), and if it's a physical device (1<<1).
    const SDL_AudioDeviceID flags = (iscapture ? 0 : (1<<0)) | (islogical ? 0 : (1<<1));

    const SDL_AudioDeviceID instance_id = (((SDL_AudioDeviceID) (SDL_AtomicIncRef(&current_audio.last_device_instance_id) + 1)) << 2) | flags;
    SDL_assert( (instance_id >= 2) && (instance_id < SDL_AUDIO_DEVICE_DEFAULT_CAPTURE) );
    return instance_id;
}

// this assumes you hold the _physical_ device lock for this logical device! This will not unlock the lock or close the physical device!
//  It also will not unref the physical device, since we might be shutting down; SDL_CloseAudioDevice handles the unref.
static void DestroyLogicalAudioDevice(SDL_LogicalAudioDevice *logdev)
{
    // Remove ourselves from the device_hash hashtable.
    if (current_audio.device_hash) {  // will be NULL while shutting down.
        SDL_LockRWLockForWriting(current_audio.device_hash_lock);
        SDL_RemoveFromHashTable(current_audio.device_hash, (const void *) (uintptr_t) logdev->instance_id);
        SDL_UnlockRWLock(current_audio.device_hash_lock);
    }

    // remove ourselves from the physical device's list of logical devices.
    if (logdev->next) {
        logdev->next->prev = logdev->prev;
    }
    if (logdev->prev) {
        logdev->prev->next = logdev->next;
    }
    if (logdev->physical_device->logical_devices == logdev) {
        logdev->physical_device->logical_devices = logdev->next;
    }

    // unbind any still-bound streams...
    SDL_AudioStream *next;
    for (SDL_AudioStream *stream = logdev->bound_streams; stream != NULL; stream = next) {
        SDL_LockMutex(stream->lock);
        next = stream->next_binding;
        stream->next_binding = NULL;
        stream->prev_binding = NULL;
        stream->bound_device = NULL;
        SDL_UnlockMutex(stream->lock);
    }

    UpdateAudioStreamFormatsPhysical(logdev->physical_device);
    SDL_free(logdev);
}

// this must not be called while `device` is still in a device list, or while a device's audio thread is still running.
static void DestroyPhysicalAudioDevice(SDL_AudioDevice *device)
{
    if (!device) {
        return;
    }

    // Destroy any logical devices that still exist...
    SDL_LockMutex(device->lock);
    while (device->logical_devices != NULL) {
        DestroyLogicalAudioDevice(device->logical_devices);
    }
    SDL_UnlockMutex(device->lock);

    // it's safe to not hold the lock for this (we can't anyhow, or the audio thread won't quit), because we shouldn't be in the device list at this point.
    ClosePhysicalAudioDevice(device);

    current_audio.impl.FreeDeviceHandle(device);

    SDL_DestroyMutex(device->lock);
    SDL_free(device->work_buffer);
    SDL_free(device->name);
    SDL_free(device);
}

// Don't hold the device lock when calling this, as we may destroy the device!
void UnrefPhysicalAudioDevice(SDL_AudioDevice *device)
{
    if (SDL_AtomicDecRef(&device->refcount)) {
        // take it out of the device list.
        SDL_LockRWLockForWriting(current_audio.device_hash_lock);
        if (SDL_RemoveFromHashTable(current_audio.device_hash, (const void *) (uintptr_t) device->instance_id)) {
            SDL_AtomicAdd(device->iscapture ? &current_audio.capture_device_count : &current_audio.output_device_count, -1);
        }
        SDL_UnlockRWLock(current_audio.device_hash_lock);
        DestroyPhysicalAudioDevice(device);  // ...and nuke it.
    }
}

void RefPhysicalAudioDevice(SDL_AudioDevice *device)
{
    SDL_AtomicIncRef(&device->refcount);
}

static SDL_AudioDevice *CreatePhysicalAudioDevice(const char *name, SDL_bool iscapture, const SDL_AudioSpec *spec, void *handle, SDL_AtomicInt *device_count)
{
    SDL_assert(name != NULL);

    SDL_LockRWLockForReading(current_audio.device_hash_lock);
    const int shutting_down = SDL_AtomicGet(&current_audio.shutting_down);
    SDL_UnlockRWLock(current_audio.device_hash_lock);
    if (shutting_down) {
        return NULL;  // we're shutting down, don't add any devices that are hotplugged at the last possible moment.
    }

    SDL_AudioDevice *device = (SDL_AudioDevice *)SDL_calloc(1, sizeof(SDL_AudioDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }

    device->name = SDL_strdup(name);
    if (!device->name) {
        SDL_free(device);
        SDL_OutOfMemory();
        return NULL;
    }

    device->lock = SDL_CreateMutex();
    if (!device->lock) {
        SDL_free(device->name);
        SDL_free(device);
        return NULL;
    }

    SDL_AtomicSet(&device->shutdown, 0);
    SDL_AtomicSet(&device->zombie, 0);
    device->iscapture = iscapture;
    SDL_copyp(&device->spec, spec);
    SDL_copyp(&device->default_spec, spec);
    device->sample_frames = GetDefaultSampleFramesFromFreq(device->spec.freq);
    device->silence_value = SDL_GetSilenceValueForFormat(device->spec.format);
    device->handle = handle;

    device->instance_id = AssignAudioDeviceInstanceId(iscapture, /*islogical=*/SDL_FALSE);

    SDL_LockRWLockForWriting(current_audio.device_hash_lock);
    if (SDL_InsertIntoHashTable(current_audio.device_hash, (const void *) (uintptr_t) device->instance_id, device)) {
        SDL_AtomicAdd(device_count, 1);
    } else {
        SDL_free(device->name);
        SDL_free(device);
        device = NULL;
    }
    SDL_UnlockRWLock(current_audio.device_hash_lock);

    RefPhysicalAudioDevice(device);  // unref'd on device disconnect.
    return device;
}

static SDL_AudioDevice *CreateAudioCaptureDevice(const char *name, const SDL_AudioSpec *spec, void *handle)
{
    SDL_assert(current_audio.impl.HasCaptureSupport);
    return CreatePhysicalAudioDevice(name, SDL_TRUE, spec, handle, &current_audio.capture_device_count);
}

static SDL_AudioDevice *CreateAudioOutputDevice(const char *name, const SDL_AudioSpec *spec, void *handle)
{
    return CreatePhysicalAudioDevice(name, SDL_FALSE, spec, handle, &current_audio.output_device_count);
}

// The audio backends call this when a new device is plugged in.
SDL_AudioDevice *SDL_AddAudioDevice(const SDL_bool iscapture, const char *name, const SDL_AudioSpec *inspec, void *handle)
{
    const SDL_AudioFormat default_format = iscapture ? DEFAULT_AUDIO_CAPTURE_FORMAT : DEFAULT_AUDIO_OUTPUT_FORMAT;
    const int default_channels = iscapture ? DEFAULT_AUDIO_CAPTURE_CHANNELS : DEFAULT_AUDIO_OUTPUT_CHANNELS;
    const int default_freq = iscapture ? DEFAULT_AUDIO_CAPTURE_FREQUENCY : DEFAULT_AUDIO_OUTPUT_FREQUENCY;

    SDL_AudioSpec spec;
    if (!inspec) {
        spec.format = default_format;
        spec.channels = default_channels;
        spec.freq = default_freq;
    } else {
        spec.format = (inspec->format != 0) ? inspec->format : default_format;
        spec.channels = (inspec->channels != 0) ? inspec->channels : default_channels;
        spec.freq = (inspec->freq != 0) ? inspec->freq : default_freq;
    }

    SDL_AudioDevice *device = iscapture ? CreateAudioCaptureDevice(name, &spec, handle) : CreateAudioOutputDevice(name, &spec, handle);
    if (device) {
        // Post the event, if desired
        if (SDL_EventEnabled(SDL_EVENT_AUDIO_DEVICE_ADDED)) {
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_EVENT_AUDIO_DEVICE_ADDED;
            event.adevice.which = device->instance_id;
            event.adevice.iscapture = iscapture;
            SDL_PushEvent(&event);
        }
    }

    return device;
}

// Called when a device is removed from the system, or it fails unexpectedly, from any thread, possibly even the audio device's thread.
void SDL_AudioDeviceDisconnected(SDL_AudioDevice *device)
{
    if (!device) {
        return;
    }

    SDL_LockMutex(device->lock);

    if (!SDL_AtomicCAS(&device->zombie, 0, 1)) {
        SDL_UnlockMutex(device->lock);
        return;  // already disconnected this device, don't do it twice.
    }

    // Swap in "Zombie" versions of the usual platform interfaces, so the device will keep
    // making progress until the app closes it. Otherwise, streams might continue to
    // accumulate waste data that never drains, apps that depend on audio callbacks to
    // progress will freeze, etc.
    device->WaitDevice = ZombieWaitDevice;
    device->GetDeviceBuf = ZombieGetDeviceBuf;
    device->PlayDevice = ZombiePlayDevice;
    device->WaitCaptureDevice = ZombieWaitDevice;
    device->CaptureFromDevice = ZombieCaptureFromDevice;
    device->FlushCapture = ZombieFlushCapture;

    const SDL_AudioDeviceID devid = device->instance_id;
    const SDL_bool is_default_device = ((devid == current_audio.default_output_device_id) || (devid == current_audio.default_capture_device_id)) ? SDL_TRUE : SDL_FALSE;

    // get a count of all devices currently attached. Save these off in an array so we can
    //  send events for each after we're done with the device lock, in case something tries
    //  to close a device from an event filter, as this would deadlock waiting on the device
    //  thread to join, which will be waiting for the device lock, too.
    int total_devices = 0;
    SDL_bool isstack = SDL_FALSE;
    SDL_AudioDeviceID *devices = NULL;
    if (SDL_EventEnabled(SDL_EVENT_AUDIO_DEVICE_REMOVED)) {
        total_devices++;  // count the physical device.
        for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
            total_devices++;
        }

        devices = SDL_small_alloc(SDL_AudioDeviceID, total_devices, &isstack);
        int deviceidx = 0;

        if (devices) {  // if we ran out of memory, we won't send disconnect events, but you probably have deeper problems anyhow.
            if (is_default_device) {
                // dump any logical devices that explicitly opened this device. Things that opened the system default can stay.
                for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
                    if (!logdev->opened_as_default) {  // if opened as a default, leave it on the zombie device for later migration.
                        SDL_assert(deviceidx < total_devices);
                        devices[deviceidx++] = logdev->instance_id;
                    }
                }
            } else {
                // report _all_ logical devices as disconnected.
                for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
                    SDL_assert(deviceidx < total_devices);
                    devices[deviceidx++] = logdev->instance_id;
                }
            }

            SDL_assert(deviceidx < total_devices);
            devices[deviceidx++] = device->instance_id;
            total_devices = deviceidx;
        }
    }

    // Let go of the lock before sending events, so an event filter trying to close a device won't deadlock with the device thread.
    SDL_UnlockMutex(device->lock);

    if (devices) {  // NULL if event is disabled or disaster struck.
        const Uint8 iscapture = device->iscapture ? 1 : 0;
        for (int i = 0; i < total_devices; i++) {
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_EVENT_AUDIO_DEVICE_REMOVED;
            event.adevice.which = devices[i];
            event.adevice.iscapture = iscapture;
            SDL_PushEvent(&event);
        }
        SDL_small_free(devices, isstack);
    }

    // Is this a non-default device? We can unref it now.
    // Otherwise, we'll unref it when a new default device is chosen.
    if (!is_default_device) {
        UnrefPhysicalAudioDevice(device);
    }
}


// stubs for audio drivers that don't need a specific entry point...

static void SDL_AudioThreadDeinit_Default(SDL_AudioDevice *device) { /* no-op. */ }
static int SDL_AudioWaitDevice_Default(SDL_AudioDevice *device) { return 0; /* no-op. */ }
static int SDL_AudioPlayDevice_Default(SDL_AudioDevice *device, const Uint8 *buffer, int buffer_size) { return 0; /* no-op. */ }
static int SDL_AudioWaitCaptureDevice_Default(SDL_AudioDevice *device) { return 0; /* no-op. */ }
static void SDL_AudioFlushCapture_Default(SDL_AudioDevice *device) { /* no-op. */ }
static void SDL_AudioCloseDevice_Default(SDL_AudioDevice *device) { /* no-op. */ }
static void SDL_AudioDeinitializeStart_Default(void) { /* no-op. */ }
static void SDL_AudioDeinitialize_Default(void) { /* no-op. */ }
static void SDL_AudioFreeDeviceHandle_Default(SDL_AudioDevice *device) { /* no-op. */ }

static void SDL_AudioThreadInit_Default(SDL_AudioDevice *device)
{
    SDL_SetThreadPriority(device->iscapture ? SDL_THREAD_PRIORITY_HIGH : SDL_THREAD_PRIORITY_TIME_CRITICAL);
}

static void SDL_AudioDetectDevices_Default(SDL_AudioDevice **default_output, SDL_AudioDevice **default_capture)
{
    // you have to write your own implementation if these assertions fail.
    SDL_assert(current_audio.impl.OnlyHasDefaultOutputDevice);
    SDL_assert(current_audio.impl.OnlyHasDefaultCaptureDevice || !current_audio.impl.HasCaptureSupport);

    *default_output = SDL_AddAudioDevice(SDL_FALSE, DEFAULT_OUTPUT_DEVNAME, NULL, (void *)((size_t)0x1));
    if (current_audio.impl.HasCaptureSupport) {
        *default_capture = SDL_AddAudioDevice(SDL_TRUE, DEFAULT_INPUT_DEVNAME, NULL, (void *)((size_t)0x2));
    }
}

static Uint8 *SDL_AudioGetDeviceBuf_Default(SDL_AudioDevice *device, int *buffer_size)
{
    *buffer_size = 0;
    return NULL;
}

static int SDL_AudioCaptureFromDevice_Default(SDL_AudioDevice *device, void *buffer, int buflen)
{
    return SDL_Unsupported();
}

static int SDL_AudioOpenDevice_Default(SDL_AudioDevice *device)
{
    return SDL_Unsupported();
}

// Fill in stub functions for unused driver entry points. This lets us blindly call them without having to check for validity first.
static void CompleteAudioEntryPoints(void)
{
    #define FILL_STUB(x) if (!current_audio.impl.x) { current_audio.impl.x = SDL_Audio##x##_Default; }
    FILL_STUB(DetectDevices);
    FILL_STUB(OpenDevice);
    FILL_STUB(ThreadInit);
    FILL_STUB(ThreadDeinit);
    FILL_STUB(WaitDevice);
    FILL_STUB(PlayDevice);
    FILL_STUB(GetDeviceBuf);
    FILL_STUB(WaitCaptureDevice);
    FILL_STUB(CaptureFromDevice);
    FILL_STUB(FlushCapture);
    FILL_STUB(CloseDevice);
    FILL_STUB(FreeDeviceHandle);
    FILL_STUB(DeinitializeStart);
    FILL_STUB(Deinitialize);
    #undef FILL_STUB
}

static SDL_AudioDeviceID GetFirstAddedAudioDeviceID(const SDL_bool iscapture)
{
    SDL_AudioDeviceID retval = (SDL_AudioDeviceID) SDL_AUDIO_DEVICE_DEFAULT_OUTPUT;  // According to AssignAudioDeviceInstanceId, nothing can have a value this large.

    // (Device IDs increase as new devices are added, so the first device added has the lowest SDL_AudioDeviceID value.)
    SDL_LockRWLockForReading(current_audio.device_hash_lock);

    const void *key;
    const void *value;
    void *iter = NULL;
    while (SDL_IterateHashTable(current_audio.device_hash, &key, &value, &iter)) {
        const SDL_AudioDeviceID devid = (SDL_AudioDeviceID) (uintptr_t) value;
        // bit #1 of devid is set for physical devices and unset for logical.
        const SDL_bool isphysical = (devid & (1<<1)) ? SDL_TRUE : SDL_FALSE;
        if (isphysical && (devid < retval)) {
            retval = devid;
        }
    }

    SDL_UnlockRWLock(current_audio.device_hash_lock);
    return (retval == SDL_AUDIO_DEVICE_DEFAULT_OUTPUT) ? 0 : retval;
}

static Uint32 HashAudioDeviceID(const void *key, void *data)
{
    // shift right 2, to dump the first two bits, since these are flags
    //  (capture vs playback, logical vs physical) and the rest are unique incrementing integers.
    return ((Uint32) ((uintptr_t) key)) >> 2;
}

static SDL_bool MatchAudioDeviceID(const void *a, const void *b, void *data)
{
    return (a == b) ? SDL_TRUE : SDL_FALSE;  // they're simple Uint32 values cast to pointers.
}

static void NukeAudioDeviceHashItem(const void *key, const void *value, void *data)
{
    // no-op, keys and values in this hashtable are treated as Plain Old Data and don't get freed here.
}

// !!! FIXME: the video subsystem does SDL_VideoInit, not SDL_InitVideo. Make this match.
int SDL_InitAudio(const char *driver_name)
{
    if (SDL_GetCurrentAudioDriver()) {
        SDL_QuitAudio(); // shutdown driver if already running.
    }

    SDL_ChooseAudioConverters();
    SDL_SetupAudioResampler();

    SDL_RWLock *device_hash_lock = SDL_CreateRWLock();  // create this early, so if it fails we don't have to tear down the whole audio subsystem.
    if (!device_hash_lock) {
        return -1;
    }

    SDL_HashTable *device_hash = SDL_CreateHashTable(NULL, 8, HashAudioDeviceID, MatchAudioDeviceID, NukeAudioDeviceHashItem, SDL_FALSE);
    if (!device_hash) {
        SDL_DestroyRWLock(device_hash_lock);
        return -1;
    }


    // Select the proper audio driver
    if (driver_name == NULL) {
        driver_name = SDL_GetHint(SDL_HINT_AUDIO_DRIVER);
    }

    SDL_bool initialized = SDL_FALSE;
    SDL_bool tried_to_init = SDL_FALSE;

    if (driver_name != NULL && *driver_name != 0) {
        char *driver_name_copy = SDL_strdup(driver_name);
        const char *driver_attempt = driver_name_copy;

        if (driver_name_copy == NULL) {
            SDL_DestroyRWLock(device_hash_lock);
            SDL_DestroyHashTable(device_hash);
            return SDL_OutOfMemory();
        }

        while (driver_attempt != NULL && *driver_attempt != 0 && !initialized) {
            char *driver_attempt_end = SDL_strchr(driver_attempt, ',');
            if (driver_attempt_end != NULL) {
                *driver_attempt_end = '\0';
            }

            // SDL 1.2 uses the name "dsound", so we'll support both.
            if (SDL_strcmp(driver_attempt, "dsound") == 0) {
                driver_attempt = "directsound";
            } else if (SDL_strcmp(driver_attempt, "pulse") == 0) {  // likewise, "pulse" was renamed to "pulseaudio"
                driver_attempt = "pulseaudio";
            }

            for (int i = 0; bootstrap[i]; ++i) {
                if (SDL_strcasecmp(bootstrap[i]->name, driver_attempt) == 0) {
                    tried_to_init = SDL_TRUE;
                    SDL_zero(current_audio);
                    SDL_AtomicSet(&current_audio.last_device_instance_id, 2);  // start past 1 because of SDL2's legacy interface.
                    current_audio.device_hash_lock = device_hash_lock;
                    current_audio.device_hash = device_hash;
                    if (bootstrap[i]->init(&current_audio.impl)) {
                        current_audio.name = bootstrap[i]->name;
                        current_audio.desc = bootstrap[i]->desc;
                        initialized = SDL_TRUE;
                    }
                    break;
                }
            }

            driver_attempt = (driver_attempt_end != NULL) ? (driver_attempt_end + 1) : NULL;
        }

        SDL_free(driver_name_copy);
    } else {
        for (int i = 0; (!initialized) && (bootstrap[i]); ++i) {
            if (bootstrap[i]->demand_only) {
                continue;
            }

            tried_to_init = SDL_TRUE;
            SDL_zero(current_audio);
            SDL_AtomicSet(&current_audio.last_device_instance_id, 2);  // start past 1 because of SDL2's legacy interface.
            current_audio.device_hash_lock = device_hash_lock;
            current_audio.device_hash = device_hash;
            if (bootstrap[i]->init(&current_audio.impl)) {
                current_audio.name = bootstrap[i]->name;
                current_audio.desc = bootstrap[i]->desc;
                initialized = SDL_TRUE;
            }
        }
    }

    if (!initialized) {
        // specific drivers will set the error message if they fail, but otherwise we do it here.
        if (!tried_to_init) {
            if (driver_name) {
                SDL_SetError("Audio target '%s' not available", driver_name);
            } else {
                SDL_SetError("No available audio device");
            }
        }

        SDL_zero(current_audio);
        SDL_DestroyRWLock(device_hash_lock);
        SDL_DestroyHashTable(device_hash);
        current_audio.device_hash_lock = NULL;
        current_audio.device_hash = NULL;
        return -1;  // No driver was available, so fail.
    }

    CompleteAudioEntryPoints();

    // Make sure we have a list of devices available at startup...
    SDL_AudioDevice *default_output = NULL;
    SDL_AudioDevice *default_capture = NULL;
    current_audio.impl.DetectDevices(&default_output, &default_capture);

    // these are only set if default_* is non-NULL, in case the backend just called SDL_DefaultAudioDeviceChanged directly during DetectDevices.
    if (default_output) {
        current_audio.default_output_device_id = default_output->instance_id;
    }
    if (default_capture) {
        current_audio.default_capture_device_id = default_capture->instance_id;
    }

    // If no default was _ever_ specified, just take the first device we see, if any.
    if (!current_audio.default_output_device_id) {
        current_audio.default_output_device_id = GetFirstAddedAudioDeviceID(/*iscapture=*/SDL_FALSE);
    }
    if (!current_audio.default_capture_device_id) {
        current_audio.default_capture_device_id = GetFirstAddedAudioDeviceID(/*iscapture=*/SDL_TRUE);
    }

    return 0;
}

void SDL_QuitAudio(void)
{
    if (!current_audio.name) {  // not initialized?!
        return;
    }

    current_audio.impl.DeinitializeStart();

    // Destroy any audio streams that still exist...
    while (current_audio.existing_streams != NULL) {
        SDL_DestroyAudioStream(current_audio.existing_streams);
    }

    SDL_LockRWLockForWriting(current_audio.device_hash_lock);
    SDL_AtomicSet(&current_audio.shutting_down, 1);
    SDL_HashTable *device_hash = current_audio.device_hash;
    current_audio.device_hash = NULL;
    SDL_AtomicSet(&current_audio.output_device_count, 0);
    SDL_AtomicSet(&current_audio.capture_device_count, 0);
    SDL_UnlockRWLock(current_audio.device_hash_lock);

    const void *key;
    const void *value;
    void *iter = NULL;
    while (SDL_IterateHashTable(device_hash, &key, &value, &iter)) {
        // bit #1 of devid is set for physical devices and unset for logical.
        const SDL_AudioDeviceID devid = (SDL_AudioDeviceID) (uintptr_t) key;
        const SDL_bool isphysical = (devid & (1<<1)) ? SDL_TRUE : SDL_FALSE;
        if (isphysical) {
            DestroyPhysicalAudioDevice((SDL_AudioDevice *) value);
        }
    }

    // Free the driver data
    current_audio.impl.Deinitialize();

    SDL_DestroyRWLock(current_audio.device_hash_lock);
    SDL_DestroyHashTable(device_hash);

    SDL_zero(current_audio);
}


void SDL_AudioThreadFinalize(SDL_AudioDevice *device)
{
    UnrefPhysicalAudioDevice(device);
}

static void MixFloat32Audio(float *dst, const float *src, const int buffer_size)
{
    if (SDL_MixAudioFormat((Uint8 *) dst, (const Uint8 *) src, SDL_AUDIO_F32, buffer_size, SDL_MIX_MAXVOLUME) < 0) {
        SDL_assert(!"This shouldn't happen.");
    }
}


// Output device thread. This is split into chunks, so backends that need to control this directly can use the pieces they need without duplicating effort.

void SDL_OutputAudioThreadSetup(SDL_AudioDevice *device)
{
    SDL_assert(!device->iscapture);
    RefPhysicalAudioDevice(device);  // unref'd when the audio thread terminates (ProvidesOwnCallbackThread implementations should call SDL_AudioThreadFinalize appropriately).
    current_audio.impl.ThreadInit(device);
}

SDL_bool SDL_OutputAudioThreadIterate(SDL_AudioDevice *device)
{
    SDL_assert(!device->iscapture);

    SDL_LockMutex(device->lock);

    if (SDL_AtomicGet(&device->shutdown)) {
        SDL_UnlockMutex(device->lock);
        return SDL_FALSE;  // we're done, shut it down.
    }

    SDL_bool failed = SDL_FALSE;
    int buffer_size = device->buffer_size;
    Uint8 *device_buffer = device->GetDeviceBuf(device, &buffer_size);
    if (buffer_size == 0) {
        // WASAPI (maybe others, later) does this to say "just abandon this iteration and try again next time."
    } else if (!device_buffer) {
        failed = SDL_TRUE;
    } else {
        SDL_assert(buffer_size <= device->buffer_size);  // you can ask for less, but not more.
        SDL_assert(AudioDeviceCanUseSimpleCopy(device) == device->simple_copy);  // make sure this hasn't gotten out of sync.

        // can we do a basic copy without silencing/mixing the buffer? This is an extremely likely scenario, so we special-case it.
        if (device->simple_copy) {
            SDL_LogicalAudioDevice *logdev = device->logical_devices;
            SDL_AudioStream *stream = logdev->bound_streams;

            // We should have updated this elsewhere if the format changed!
            SDL_assert(AUDIO_SPECS_EQUAL(stream->dst_spec, device->spec));

            const int br = SDL_AtomicGet(&logdev->paused) ? 0 : SDL_GetAudioStreamData(stream, device_buffer, buffer_size);
            if (br < 0) {  // Probably OOM. Kill the audio device; the whole thing is likely dying soon anyhow.
                failed = SDL_TRUE;
                SDL_memset(device_buffer, device->silence_value, buffer_size);  // just supply silence to the device before we die.
            } else if (br < buffer_size) {
                SDL_memset(device_buffer + br, device->silence_value, buffer_size - br);  // silence whatever we didn't write to.
            }
        } else {  // need to actually mix (or silence the buffer)
            float *final_mix_buffer = (float *) ((device->spec.format == SDL_AUDIO_F32) ? device_buffer : device->mix_buffer);
            const int needed_samples = buffer_size / SDL_AUDIO_BYTESIZE(device->spec.format);
            const int work_buffer_size = needed_samples * sizeof (float);
            SDL_AudioSpec outspec;

            SDL_assert(work_buffer_size <= device->work_buffer_size);

            outspec.format = SDL_AUDIO_F32;
            outspec.channels = device->spec.channels;
            outspec.freq = device->spec.freq;

            SDL_memset(final_mix_buffer, '\0', work_buffer_size);  // start with silence.

            for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
                if (SDL_AtomicGet(&logdev->paused)) {
                    continue;  // paused? Skip this logical device.
                }

                const SDL_AudioPostmixCallback postmix = logdev->postmix;
                float *mix_buffer = final_mix_buffer;
                if (postmix) {
                    mix_buffer = device->postmix_buffer;
                    SDL_memset(mix_buffer, '\0', work_buffer_size);  // start with silence.
                }

                for (SDL_AudioStream *stream = logdev->bound_streams; stream != NULL; stream = stream->next_binding) {
                    // We should have updated this elsewhere if the format changed!
                    SDL_assert(AUDIO_SPECS_EQUAL(stream->dst_spec, outspec));

                    /* this will hold a lock on `stream` while getting. We don't explicitly lock the streams
                       for iterating here because the binding linked list can only change while the device lock is held.
                       (we _do_ lock the stream during binding/unbinding to make sure that two threads can't try to bind
                       the same stream to different devices at the same time, though.) */
                    const int br = SDL_GetAudioStreamData(stream, device->work_buffer, work_buffer_size);
                    if (br < 0) {  // Probably OOM. Kill the audio device; the whole thing is likely dying soon anyhow.
                        failed = SDL_TRUE;
                        break;
                    } else if (br > 0) {  // it's okay if we get less than requested, we mix what we have.
                        MixFloat32Audio(mix_buffer, (float *) device->work_buffer, br);
                    }
                }

                if (postmix) {
                    SDL_assert(mix_buffer == device->postmix_buffer);
                    postmix(logdev->postmix_userdata, &outspec, mix_buffer, work_buffer_size);
                    MixFloat32Audio(final_mix_buffer, mix_buffer, work_buffer_size);
                }
            }

            if (((Uint8 *) final_mix_buffer) != device_buffer) {
                // !!! FIXME: we can't promise the device buf is aligned/padded for SIMD.
                //ConvertAudio(needed_samples * device->spec.channels, final_mix_buffer, SDL_AUDIO_F32, device->spec.channels, device_buffer, device->spec.format, device->spec.channels, device->work_buffer);
                ConvertAudio(needed_samples / device->spec.channels, final_mix_buffer, SDL_AUDIO_F32, device->spec.channels, device->work_buffer, device->spec.format, device->spec.channels, NULL);
                SDL_memcpy(device_buffer, device->work_buffer, buffer_size);
            }
        }

        // PlayDevice SHOULD NOT BLOCK, as we are holding a lock right now. Block in WaitDevice instead!
        if (device->PlayDevice(device, device_buffer, buffer_size) < 0) {
            failed = SDL_TRUE;
        }
    }

    SDL_UnlockMutex(device->lock);

    if (failed) {
        SDL_AudioDeviceDisconnected(device);  // doh.
    }

    return SDL_TRUE;  // always go on if not shutting down, even if device failed.
}

void SDL_OutputAudioThreadShutdown(SDL_AudioDevice *device)
{
    SDL_assert(!device->iscapture);
    const int frames = device->buffer_size / SDL_AUDIO_FRAMESIZE(device->spec);
    // Wait for the audio to drain if device didn't die.
    if (!SDL_AtomicGet(&device->zombie)) {
        SDL_Delay(((frames * 1000) / device->spec.freq) * 2);
    }
    current_audio.impl.ThreadDeinit(device);
    SDL_AudioThreadFinalize(device);
}

static int SDLCALL OutputAudioThread(void *devicep)  // thread entry point
{
    SDL_AudioDevice *device = (SDL_AudioDevice *)devicep;
    SDL_assert(device != NULL);
    SDL_assert(!device->iscapture);
    SDL_OutputAudioThreadSetup(device);

    do {
        if (device->WaitDevice(device) < 0) {
            SDL_AudioDeviceDisconnected(device);  // doh. (but don't break out of the loop, just be a zombie for now!)
        }
    } while (SDL_OutputAudioThreadIterate(device));

    SDL_OutputAudioThreadShutdown(device);
    return 0;
}



// Capture device thread. This is split into chunks, so backends that need to control this directly can use the pieces they need without duplicating effort.

void SDL_CaptureAudioThreadSetup(SDL_AudioDevice *device)
{
    SDL_assert(device->iscapture);
    RefPhysicalAudioDevice(device);  // unref'd when the audio thread terminates (ProvidesOwnCallbackThread implementations should call SDL_AudioThreadFinalize appropriately).
    current_audio.impl.ThreadInit(device);
}

SDL_bool SDL_CaptureAudioThreadIterate(SDL_AudioDevice *device)
{
    SDL_assert(device->iscapture);

    SDL_LockMutex(device->lock);

    if (SDL_AtomicGet(&device->shutdown)) {
        SDL_UnlockMutex(device->lock);
        return SDL_FALSE;  // we're done, shut it down.
    }

    SDL_bool failed = SDL_FALSE;

    if (device->logical_devices == NULL) {
        device->FlushCapture(device); // nothing wants data, dump anything pending.
    } else {
        // this SHOULD NOT BLOCK, as we are holding a lock right now. Block in WaitCaptureDevice!
        int br = device->CaptureFromDevice(device, device->work_buffer, device->buffer_size);
        if (br < 0) {  // uhoh, device failed for some reason!
            failed = SDL_TRUE;
        } else if (br > 0) {  // queue the new data to each bound stream.
            for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
                if (SDL_AtomicGet(&logdev->paused)) {
                    continue;  // paused? Skip this logical device.
                }

                void *output_buffer = device->work_buffer;

                // I don't know why someone would want a postmix on a capture device, but we offer it for API consistency.
                if (logdev->postmix) {
                    // move to float format.
                    SDL_AudioSpec outspec;
                    outspec.format = SDL_AUDIO_F32;
                    outspec.channels = device->spec.channels;
                    outspec.freq = device->spec.freq;
                    output_buffer = device->postmix_buffer;
                    const int frames = br / SDL_AUDIO_FRAMESIZE(device->spec);
                    br = frames * SDL_AUDIO_FRAMESIZE(outspec);
                    ConvertAudio(frames, device->work_buffer, device->spec.format, outspec.channels, device->postmix_buffer, SDL_AUDIO_F32, outspec.channels, NULL);
                    logdev->postmix(logdev->postmix_userdata, &outspec, device->postmix_buffer, br);
                }

                for (SDL_AudioStream *stream = logdev->bound_streams; stream != NULL; stream = stream->next_binding) {
                    // We should have updated this elsewhere if the format changed!
                    SDL_assert(stream->src_spec.format == (logdev->postmix ? SDL_AUDIO_F32 : device->spec.format));
                    SDL_assert(stream->src_spec.channels == device->spec.channels);
                    SDL_assert(stream->src_spec.freq == device->spec.freq);

                    /* this will hold a lock on `stream` while putting. We don't explicitly lock the streams
                       for iterating here because the binding linked list can only change while the device lock is held.
                       (we _do_ lock the stream during binding/unbinding to make sure that two threads can't try to bind
                       the same stream to different devices at the same time, though.) */
                    if (SDL_PutAudioStreamData(stream, output_buffer, br) < 0) {
                        // oh crud, we probably ran out of memory. This is possibly an overreaction to kill the audio device, but it's likely the whole thing is going down in a moment anyhow.
                        failed = SDL_TRUE;
                        break;
                    }
                }
            }
        }
    }

    SDL_UnlockMutex(device->lock);

    if (failed) {
        SDL_AudioDeviceDisconnected(device);  // doh.
    }

    return SDL_TRUE;  // always go on if not shutting down, even if device failed.
}

void SDL_CaptureAudioThreadShutdown(SDL_AudioDevice *device)
{
    SDL_assert(device->iscapture);
    device->FlushCapture(device);
    current_audio.impl.ThreadDeinit(device);
    SDL_AudioThreadFinalize(device);
}

static int SDLCALL CaptureAudioThread(void *devicep)  // thread entry point
{
    SDL_AudioDevice *device = (SDL_AudioDevice *)devicep;
    SDL_assert(device != NULL);
    SDL_assert(device->iscapture);
    SDL_CaptureAudioThreadSetup(device);

    do {
        if (device->WaitCaptureDevice(device) < 0) {
            SDL_AudioDeviceDisconnected(device);  // doh. (but don't break out of the loop, just be a zombie for now!)
        }
    } while (SDL_CaptureAudioThreadIterate(device));

    SDL_CaptureAudioThreadShutdown(device);
    return 0;
}


static SDL_AudioDeviceID *GetAudioDevices(int *reqcount, SDL_bool iscapture)
{
    if (!SDL_GetCurrentAudioDriver()) {
        SDL_SetError("Audio subsystem is not initialized");
        return NULL;
    }

    SDL_AudioDeviceID *retval = NULL;

    SDL_LockRWLockForReading(current_audio.device_hash_lock);
    int num_devices = SDL_AtomicGet(iscapture ? &current_audio.capture_device_count : &current_audio.output_device_count);
    if (num_devices > 0) {
        retval = (SDL_AudioDeviceID *) SDL_malloc((num_devices + 1) * sizeof (SDL_AudioDeviceID));
        if (retval == NULL) {
            num_devices = 0;
            SDL_OutOfMemory();
        } else {
            int devs_seen = 0;
            const void *key;
            const void *value;
            void *iter = NULL;
            while (SDL_IterateHashTable(current_audio.device_hash, &key, &value, &iter)) {
                const SDL_AudioDeviceID devid = (SDL_AudioDeviceID) (uintptr_t) key;
                // bit #0 of devid is set for output devices and unset for capture.
                // bit #1 of devid is set for physical devices and unset for logical.
                const SDL_bool devid_iscapture = (devid & (1<<0)) ? SDL_FALSE : SDL_TRUE;
                const SDL_bool isphysical = (devid & (1<<1)) ? SDL_TRUE : SDL_FALSE;
                if (isphysical && (devid_iscapture == iscapture)) {
                    SDL_assert(devs_seen < num_devices);
                    retval[devs_seen++] = devid;
                }
            }

            SDL_assert(devs_seen == num_devices);
            retval[devs_seen] = 0;  // null-terminated.
        }
    }
    SDL_UnlockRWLock(current_audio.device_hash_lock);

    if (reqcount != NULL) {
        *reqcount = num_devices;
    }

    return retval;
}

SDL_AudioDeviceID *SDL_GetAudioOutputDevices(int *count)
{
    return GetAudioDevices(count, SDL_FALSE);
}

SDL_AudioDeviceID *SDL_GetAudioCaptureDevices(int *count)
{
    return GetAudioDevices(count, SDL_TRUE);
}

// If found, this locks _the physical device_ this logical device is associated with, before returning.
static SDL_LogicalAudioDevice *ObtainLogicalAudioDevice(SDL_AudioDeviceID devid)
{
    if (!SDL_GetCurrentAudioDriver()) {
        SDL_SetError("Audio subsystem is not initialized");
        return NULL;
    }

    SDL_LogicalAudioDevice *logdev = NULL;

    // bit #1 of devid is set for physical devices and unset for logical.
    const SDL_bool islogical = (devid & (1<<1)) ? SDL_FALSE : SDL_TRUE;
    if (islogical) {  // don't bother looking if it's not a logical device id value.
        SDL_LockRWLockForReading(current_audio.device_hash_lock);
        SDL_FindInHashTable(current_audio.device_hash, (const void *) (uintptr_t) devid, (const void **) &logdev);
        SDL_UnlockRWLock(current_audio.device_hash_lock);
    }

    if (!logdev) {
        SDL_SetError("Invalid audio device instance ID");
    } else {
        SDL_LockMutex(logdev->physical_device->lock);
    }

    return logdev;
}

/* this finds the physical device associated with `devid` and locks it for use.
   Note that a logical device instance id will return its associated physical device! */
static SDL_AudioDevice *ObtainPhysicalAudioDevice(SDL_AudioDeviceID devid)
{
    SDL_AudioDevice *device = NULL;

    // bit #1 of devid is set for physical devices and unset for logical.
    const SDL_bool islogical = (devid & (1<<1)) ? SDL_FALSE : SDL_TRUE;
    if (islogical) {
        SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(devid);
        if (logdev) {
            device = logdev->physical_device;
        }
    } else if (!SDL_GetCurrentAudioDriver()) {  // (the `islogical` path, above,  checks this in ObtainLogicalAudioDevice.)
        SDL_SetError("Audio subsystem is not initialized");
    } else {
        SDL_LockRWLockForReading(current_audio.device_hash_lock);
        SDL_FindInHashTable(current_audio.device_hash, (const void *) (uintptr_t) devid, (const void **) &device);
        SDL_UnlockRWLock(current_audio.device_hash_lock);

        if (!device) {
            SDL_SetError("Invalid audio device instance ID");
        } else {
            SDL_LockMutex(device->lock);  // caller must unlock.
        }
    }

    return device;
}

SDL_AudioDevice *SDL_FindPhysicalAudioDeviceByCallback(SDL_bool (*callback)(SDL_AudioDevice *device, void *userdata), void *userdata)
{
    if (!SDL_GetCurrentAudioDriver()) {
        SDL_SetError("Audio subsystem is not initialized");
        return NULL;
    }

    const void *key;
    const void *value;
    void *iter = NULL;

    SDL_LockRWLockForReading(current_audio.device_hash_lock);
    while (SDL_IterateHashTable(current_audio.device_hash, &key, &value, &iter)) {
        const SDL_AudioDeviceID devid = (SDL_AudioDeviceID) (uintptr_t) key;
        // bit #1 of devid is set for physical devices and unset for logical.
        const SDL_bool isphysical = (devid & (1<<1)) ? SDL_TRUE : SDL_FALSE;
        if (isphysical) {
            SDL_AudioDevice *device = (SDL_AudioDevice *) value;
            if (callback(device, userdata)) {  // found it?
                SDL_UnlockRWLock(current_audio.device_hash_lock);
                return device;
            }
        }
    }
    SDL_UnlockRWLock(current_audio.device_hash_lock);

    SDL_SetError("Device not found");
    return NULL;
}

static SDL_bool TestDeviceHandleCallback(SDL_AudioDevice *device, void *handle)
{
    return device->handle == handle;
}

SDL_AudioDevice *SDL_FindPhysicalAudioDeviceByHandle(void *handle)
{
    return SDL_FindPhysicalAudioDeviceByCallback(TestDeviceHandleCallback, handle);
}

char *SDL_GetAudioDeviceName(SDL_AudioDeviceID devid)
{
    SDL_AudioDevice *device = ObtainPhysicalAudioDevice(devid);
    if (!device) {
        return NULL;
    }

    char *retval = SDL_strdup(device->name);
    if (!retval) {
        SDL_OutOfMemory();
    }

    SDL_UnlockMutex(device->lock);

    return retval;
}

int SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sample_frames)
{
    if (!spec) {
        return SDL_InvalidParamError("spec");
    }

    SDL_bool wants_default = SDL_FALSE;
    if (devid == SDL_AUDIO_DEVICE_DEFAULT_OUTPUT) {
        devid = current_audio.default_output_device_id;
        wants_default = SDL_TRUE;
    } else if (devid == SDL_AUDIO_DEVICE_DEFAULT_CAPTURE) {
        devid = current_audio.default_capture_device_id;
        wants_default = SDL_TRUE;
    }

    if ((devid == 0) && wants_default) {
        return SDL_SetError("No default audio device available");
    }

    SDL_AudioDevice *device = ObtainPhysicalAudioDevice(devid);
    if (!device) {
        return -1;
    }

    SDL_copyp(spec, &device->spec);
    if (sample_frames) {
        *sample_frames = device->sample_frames;
    }
    SDL_UnlockMutex(device->lock);

    return 0;
}

// this expects the device lock to be held.  !!! FIXME: no it doesn't...?
static void ClosePhysicalAudioDevice(SDL_AudioDevice *device)
{
    SDL_AtomicSet(&device->shutdown, 1);  // alert device thread that it should terminate.

    if (device->thread != NULL) {
        if (SDL_GetThreadID(device->thread) == SDL_ThreadID()) {
            SDL_DetachThread(device->thread);  // we _are_ the device thread, just drift off into the ether when finished, nothing is waiting for us.
        } else {
            SDL_WaitThread(device->thread, NULL);   // we're not the device thread, wait for it to terminate.
        }
        device->thread = NULL;
    }

    if (device->currently_opened) {
        current_audio.impl.CloseDevice(device);  // if ProvidesOwnCallbackThread, this must join on any existing device thread before returning!
        device->currently_opened = SDL_FALSE;
        device->hidden = NULL;  // just in case.
    }

    SDL_aligned_free(device->work_buffer);
    device->work_buffer = NULL;

    SDL_aligned_free(device->mix_buffer);
    device->mix_buffer = NULL;

    SDL_aligned_free(device->postmix_buffer);
    device->postmix_buffer = NULL;

    SDL_copyp(&device->spec, &device->default_spec);
    device->sample_frames = 0;
    device->silence_value = SDL_GetSilenceValueForFormat(device->spec.format);
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID devid)
{
    SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(devid);
    if (logdev) {
        SDL_AudioDevice *device = logdev->physical_device;
        DestroyLogicalAudioDevice(logdev);
        UnrefPhysicalAudioDevice(device);  // one reference for each logical device.

        // !!! FIXME: we _need_ to release this lock, but doing so can cause a race condition if someone opens a device while we're closing it.
        SDL_UnlockMutex(device->lock);  // can't hold the lock or the audio thread will deadlock while we WaitThread it. If not closing, we're done anyhow.
        if (device->logical_devices == NULL) {  // no more logical devices? Close the physical device, too.
            ClosePhysicalAudioDevice(device);
        }
    }
}


static SDL_AudioFormat ParseAudioFormatString(const char *string)
{
    if (string) {
        #define CHECK_FMT_STRING(x) if (SDL_strcmp(string, #x) == 0) { return SDL_AUDIO_##x; }
        CHECK_FMT_STRING(U8);
        CHECK_FMT_STRING(S8);
        CHECK_FMT_STRING(S16LE);
        CHECK_FMT_STRING(S16BE);
        CHECK_FMT_STRING(S16);
        CHECK_FMT_STRING(S32LE);
        CHECK_FMT_STRING(S32BE);
        CHECK_FMT_STRING(S32);
        CHECK_FMT_STRING(F32LE);
        CHECK_FMT_STRING(F32BE);
        CHECK_FMT_STRING(F32);
        #undef CHECK_FMT_STRING
    }
    return 0;
}

static void PrepareAudioFormat(SDL_bool iscapture, SDL_AudioSpec *spec)
{
    if (spec->freq == 0) {
        spec->freq = iscapture ? DEFAULT_AUDIO_CAPTURE_FREQUENCY : DEFAULT_AUDIO_OUTPUT_FREQUENCY;

        const char *env = SDL_getenv("SDL_AUDIO_FREQUENCY");  // !!! FIXME: should be a hint?
        if (env != NULL) {
            const int val = SDL_atoi(env);
            if (val > 0) {
                spec->freq = val;
            }
        }
    }

    if (spec->channels == 0) {
        spec->channels = iscapture ? DEFAULT_AUDIO_CAPTURE_CHANNELS : DEFAULT_AUDIO_OUTPUT_CHANNELS;;
        const char *env = SDL_getenv("SDL_AUDIO_CHANNELS");
        if (env != NULL) {
            const int val = SDL_atoi(env);
            if (val > 0) {
                spec->channels = val;
            }
        }
    }

    if (spec->format == 0) {
        const SDL_AudioFormat val = ParseAudioFormatString(SDL_getenv("SDL_AUDIO_FORMAT"));
        spec->format = (val != 0) ? val : (iscapture ? DEFAULT_AUDIO_CAPTURE_FORMAT : DEFAULT_AUDIO_OUTPUT_FORMAT);
    }
}

void SDL_UpdatedAudioDeviceFormat(SDL_AudioDevice *device)
{
    device->silence_value = SDL_GetSilenceValueForFormat(device->spec.format);
    device->buffer_size = device->sample_frames * SDL_AUDIO_FRAMESIZE(device->spec);
    device->work_buffer_size = device->sample_frames * sizeof (float) * device->spec.channels;
    device->work_buffer_size = SDL_max(device->buffer_size, device->work_buffer_size);  // just in case we end up with a 64-bit audio format at some point.
}

char *SDL_GetAudioThreadName(SDL_AudioDevice *device, char *buf, size_t buflen)
{
    (void)SDL_snprintf(buf, buflen, "SDLAudio%c%d", (device->iscapture) ? 'C' : 'P', (int) device->instance_id);
    return buf;
}


// this expects the device lock to be held.
static int OpenPhysicalAudioDevice(SDL_AudioDevice *device, const SDL_AudioSpec *inspec)
{
    SDL_assert(!device->currently_opened);
    SDL_assert(device->logical_devices == NULL);

    // Just pretend to open a zombie device. It can still collect logical devices on a default device under the assumption they will all migrate when the default device is officially changed.
    if (SDL_AtomicGet(&device->zombie)) {
        return 0;  // Braaaaaaaaains.
    }

    SDL_AtomicSet(&device->shutdown, 0);  // make sure we don't kill the new thread immediately.

    // These start with the backend's implementation, but we might swap them out with zombie versions later.
    device->WaitDevice = current_audio.impl.WaitDevice;
    device->PlayDevice = current_audio.impl.PlayDevice;
    device->GetDeviceBuf = current_audio.impl.GetDeviceBuf;
    device->WaitCaptureDevice = current_audio.impl.WaitCaptureDevice;
    device->CaptureFromDevice = current_audio.impl.CaptureFromDevice;
    device->FlushCapture = current_audio.impl.FlushCapture;

    SDL_AudioSpec spec;
    SDL_copyp(&spec, inspec ? inspec : &device->default_spec);
    PrepareAudioFormat(device->iscapture, &spec);

    /* We allow the device format to change if it's better than the current settings (by various definitions of "better"). This prevents
       something low quality, like an old game using S8/8000Hz audio, from ruining a music thing playing at CD quality that tries to open later.
       (or some VoIP library that opens for mono output ruining your surround-sound game because it got there first).
       These are just requests! The backend may change any of these values during OpenDevice method! */
    device->spec.format = (SDL_AUDIO_BITSIZE(device->default_spec.format) >= SDL_AUDIO_BITSIZE(spec.format)) ? device->default_spec.format : spec.format;
    device->spec.freq = SDL_max(device->default_spec.freq, spec.freq);
    device->spec.channels = SDL_max(device->default_spec.channels, spec.channels);
    device->sample_frames = GetDefaultSampleFramesFromFreq(device->spec.freq);
    SDL_UpdatedAudioDeviceFormat(device);  // start this off sane.

    device->currently_opened = SDL_TRUE;  // mark this true even if impl.OpenDevice fails, so we know to clean up.
    if (current_audio.impl.OpenDevice(device) < 0) {
        ClosePhysicalAudioDevice(device);  // clean up anything the backend left half-initialized.
        return -1;
    }

    SDL_UpdatedAudioDeviceFormat(device);  // in case the backend changed things and forgot to call this.

    // Allocate a scratch audio buffer
    device->work_buffer = (Uint8 *)SDL_aligned_alloc(SDL_SIMDGetAlignment(), device->work_buffer_size);
    if (device->work_buffer == NULL) {
        ClosePhysicalAudioDevice(device);
        return SDL_OutOfMemory();
    }

    if (device->spec.format != SDL_AUDIO_F32) {
        device->mix_buffer = (Uint8 *)SDL_aligned_alloc(SDL_SIMDGetAlignment(), device->work_buffer_size);
        if (device->mix_buffer == NULL) {
            ClosePhysicalAudioDevice(device);
            return SDL_OutOfMemory();
        }
    }

    // Start the audio thread if necessary
    if (!current_audio.impl.ProvidesOwnCallbackThread) {
        const size_t stacksize = 0;  // just take the system default, since audio streams might have callbacks.
        char threadname[64];
        SDL_GetAudioThreadName(device, threadname, sizeof (threadname));
        device->thread = SDL_CreateThreadInternal(device->iscapture ? CaptureAudioThread : OutputAudioThread, threadname, stacksize, device);

        if (device->thread == NULL) {
            ClosePhysicalAudioDevice(device);
            return SDL_SetError("Couldn't create audio thread");
        }
    }

    return 0;
}

SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec)
{
    if (!SDL_GetCurrentAudioDriver()) {
        SDL_SetError("Audio subsystem is not initialized");
        return 0;
    }

    SDL_bool wants_default = SDL_FALSE;
    if (devid == SDL_AUDIO_DEVICE_DEFAULT_OUTPUT) {
        devid = current_audio.default_output_device_id;
        wants_default = SDL_TRUE;
    } else if (devid == SDL_AUDIO_DEVICE_DEFAULT_CAPTURE) {
        devid = current_audio.default_capture_device_id;
        wants_default = SDL_TRUE;
    }

    if ((devid == 0) && wants_default) {
        SDL_SetError("No default audio device available");
        return 0;
    }

    // this will let you use a logical device to make a new logical device on the parent physical device. Could be useful?
    SDL_AudioDevice *device = NULL;
    const SDL_bool islogical = (devid & (1<<1)) ? SDL_FALSE : SDL_TRUE;
    if (!islogical) {
        device = ObtainPhysicalAudioDevice(devid);
    } else {
        SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(devid);  // this locks the physical device, too.
        if (logdev) {
            wants_default = logdev->opened_as_default;  // was the original logical device meant to be a default? Make this one, too.
            device = logdev->physical_device;
        }
    }

    SDL_AudioDeviceID retval = 0;

    if (device) {
        SDL_LogicalAudioDevice *logdev = NULL;
        if (!wants_default && SDL_AtomicGet(&device->zombie)) {
            // uhoh, this device is undead, and just waiting to be cleaned up. Refuse explicit opens.
            SDL_SetError("Device was already lost and can't accept new opens");
        } else if ((logdev = (SDL_LogicalAudioDevice *) SDL_calloc(1, sizeof (SDL_LogicalAudioDevice))) == NULL) {
            SDL_OutOfMemory();
        } else if (!device->currently_opened && OpenPhysicalAudioDevice(device, spec) == -1) {  // first thing using this physical device? Open at the OS level...
            SDL_free(logdev);
        } else {
            RefPhysicalAudioDevice(device);  // unref'd on successful SDL_CloseAudioDevice
            SDL_AtomicSet(&logdev->paused, 0);
            retval = logdev->instance_id = AssignAudioDeviceInstanceId(device->iscapture, /*islogical=*/SDL_TRUE);
            logdev->physical_device = device;
            logdev->opened_as_default = wants_default;
            logdev->next = device->logical_devices;
            if (device->logical_devices) {
                device->logical_devices->prev = logdev;
            }
            device->logical_devices = logdev;
            UpdateAudioStreamFormatsPhysical(device);
        }
        SDL_UnlockMutex(device->lock);

        if (retval) {
            SDL_LockRWLockForWriting(current_audio.device_hash_lock);
            const SDL_bool inserted = SDL_InsertIntoHashTable(current_audio.device_hash, (const void *) (uintptr_t) retval, logdev);
            SDL_UnlockRWLock(current_audio.device_hash_lock);
            if (!inserted) {
                SDL_CloseAudioDevice(retval);
                retval = 0;
            }
        }
    }

    return retval;
}

static int SetLogicalAudioDevicePauseState(SDL_AudioDeviceID devid, int value)
{
    SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(devid);
    if (!logdev) {
        return -1;  // ObtainLogicalAudioDevice will have set an error.
    }
    SDL_AtomicSet(&logdev->paused, value);
    SDL_UnlockMutex(logdev->physical_device->lock);
    return 0;
}

int SDL_PauseAudioDevice(SDL_AudioDeviceID devid)
{
    return SetLogicalAudioDevicePauseState(devid, 1);
}

int SDLCALL SDL_ResumeAudioDevice(SDL_AudioDeviceID devid)
{
    return SetLogicalAudioDevicePauseState(devid, 0);
}

SDL_bool SDL_AudioDevicePaused(SDL_AudioDeviceID devid)
{
    SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(devid);
    SDL_bool retval = SDL_FALSE;
    if (logdev) {
        if (SDL_AtomicGet(&logdev->paused)) {
            retval = SDL_TRUE;
        }
        SDL_UnlockMutex(logdev->physical_device->lock);
    }
    return retval;
}

int SDL_SetAudioPostmixCallback(SDL_AudioDeviceID devid, SDL_AudioPostmixCallback callback, void *userdata)
{
    SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(devid);
    int retval = 0;
    if (logdev) {
        SDL_AudioDevice *device = logdev->physical_device;
        if (callback && !device->postmix_buffer) {
            device->postmix_buffer = (float *)SDL_aligned_alloc(SDL_SIMDGetAlignment(), device->work_buffer_size);
            if (device->postmix_buffer == NULL) {
                retval = SDL_OutOfMemory();
            }
        }

        if (retval == 0) {
            logdev->postmix = callback;
            logdev->postmix_userdata = userdata;

            if (device->iscapture) {
                for (SDL_AudioStream *stream = logdev->bound_streams; stream != NULL; stream = stream->next_binding) {
                    // set the proper end of the stream to the device's format.
                    // SDL_SetAudioStreamFormat does a ton of validation just to memcpy an audiospec.
                    SDL_LockMutex(stream->lock);
                    stream->src_spec.format = callback ? SDL_AUDIO_F32 : device->spec.format;
                    SDL_UnlockMutex(stream->lock);
                }
            }
        }

        UpdateAudioStreamFormatsPhysical(device);
        SDL_UnlockMutex(device->lock);
    }
    return retval;
}

int SDL_BindAudioStreams(SDL_AudioDeviceID devid, SDL_AudioStream **streams, int num_streams)
{
    const SDL_bool islogical = (devid & (1<<1)) ? SDL_FALSE : SDL_TRUE;
    SDL_LogicalAudioDevice *logdev;

    if (num_streams == 0) {
        return 0;  // nothing to do
    } else if (num_streams < 0) {
        return SDL_InvalidParamError("num_streams");
    } else if (streams == NULL) {
        return SDL_InvalidParamError("streams");
    } else if (!islogical) {
        return SDL_SetError("Audio streams are bound to device ids from SDL_OpenAudioDevice, not raw physical devices");
    } else if ((logdev = ObtainLogicalAudioDevice(devid)) == NULL) {
        return -1;  // ObtainLogicalAudioDevice set the error message.
    } else if (logdev->simplified) {
        SDL_UnlockMutex(logdev->physical_device->lock);
        return SDL_SetError("Cannot change stream bindings on device opened with SDL_OpenAudioDeviceStream");
    }

    // !!! FIXME: We'll set the device's side's format below, but maybe we should refuse to bind a stream if the app's side doesn't have a format set yet.
    // !!! FIXME: Actually, why do we allow there to be an invalid format, again?

    // make sure start of list is sane.
    SDL_assert(!logdev->bound_streams || (logdev->bound_streams->prev_binding == NULL));

    SDL_AudioDevice *device = logdev->physical_device;
    const SDL_bool iscapture = device->iscapture;
    int retval = 0;

    // lock all the streams upfront, so we can verify they aren't bound elsewhere and add them all in one block, as this is intended to add everything or nothing.
    for (int i = 0; i < num_streams; i++) {
        SDL_AudioStream *stream = streams[i];
        if (stream == NULL) {
            retval = SDL_SetError("Stream #%d is NULL", i);
        } else {
            SDL_LockMutex(stream->lock);
            SDL_assert((stream->bound_device == NULL) == ((stream->prev_binding == NULL) || (stream->next_binding == NULL)));
            if (stream->bound_device) {
                retval = SDL_SetError("Stream #%d is already bound to a device", i);
            }
        }

        if (retval != 0) {
            int j;
            for (j = 0; j <= i; j++) {
                SDL_UnlockMutex(streams[j]->lock);
            }
            break;
        }
    }

    if (retval == 0) {
        // Now that everything is verified, chain everything together.
        for (int i = 0; i < num_streams; i++) {
            SDL_AudioStream *stream = streams[i];

            stream->bound_device = logdev;
            stream->prev_binding = NULL;
            stream->next_binding = logdev->bound_streams;
            if (logdev->bound_streams) {
                logdev->bound_streams->prev_binding = stream;
            }
            logdev->bound_streams = stream;

            if (iscapture) {
                SDL_copyp(&stream->src_spec, &device->spec);
                if (logdev->postmix) {
                    stream->src_spec.format = SDL_AUDIO_F32;
                }
            }

            SDL_UnlockMutex(stream->lock);
        }
    }

    UpdateAudioStreamFormatsPhysical(device);

    SDL_UnlockMutex(device->lock);

    return retval;
}

int SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream *stream)
{
    return SDL_BindAudioStreams(devid, &stream, 1);
}

void SDL_UnbindAudioStreams(SDL_AudioStream **streams, int num_streams)
{
    /* to prevent deadlock when holding both locks, we _must_ lock the device first, and the stream second, as that is the order the audio thread will do it.
       But this means we have an unlikely, pathological case where a stream could change its binding between when we lookup its bound device and when we lock everything,
       so we double-check here. */
    for (int i = 0; i < num_streams; i++) {
        SDL_AudioStream *stream = streams[i];
        if (!stream) {
            continue;  // nothing to do, it's a NULL stream.
        }

        while (SDL_TRUE) {
            SDL_LockMutex(stream->lock);   // lock to check this and then release it, in case the device isn't locked yet.
            SDL_LogicalAudioDevice *bounddev = stream->bound_device;
            SDL_UnlockMutex(stream->lock);

            // lock in correct order.
            if (bounddev) {
                SDL_LockMutex(bounddev->physical_device->lock);  // this requires recursive mutexes, since we're likely locking the same device multiple times.
            }
            SDL_LockMutex(stream->lock);

            if (bounddev == stream->bound_device) {
                break;  // the binding didn't change in the small window where it could, so we're good.
            } else {
                SDL_UnlockMutex(stream->lock);  // it changed bindings! Try again.
                if (bounddev) {
                    SDL_UnlockMutex(bounddev->physical_device->lock);
                }
            }
        }
    }

    // everything is locked, start unbinding streams.
    for (int i = 0; i < num_streams; i++) {
        SDL_AudioStream *stream = streams[i];
        // don't allow unbinding from "simplified" devices (opened with SDL_OpenAudioDeviceStream). Just ignore them.
        if (stream && stream->bound_device && !stream->bound_device->simplified) {
            if (stream->bound_device->bound_streams == stream) {
                SDL_assert(stream->prev_binding == NULL);
                stream->bound_device->bound_streams = stream->next_binding;
            }
            if (stream->prev_binding) {
                stream->prev_binding->next_binding = stream->next_binding;
            }
            if (stream->next_binding) {
                stream->next_binding->prev_binding = stream->prev_binding;
            }
            stream->prev_binding = stream->next_binding = NULL;
        }
    }

    // Finalize and unlock everything.
    for (int i = 0; i < num_streams; i++) {
        SDL_AudioStream *stream = streams[i];
        if (stream && stream->bound_device) {
            SDL_LogicalAudioDevice *logdev = stream->bound_device;
            stream->bound_device = NULL;
            SDL_UnlockMutex(stream->lock);
            if (logdev) {
                UpdateAudioStreamFormatsPhysical(logdev->physical_device);
                SDL_UnlockMutex(logdev->physical_device->lock);
            }
        }
    }
}

void SDL_UnbindAudioStream(SDL_AudioStream *stream)
{
    SDL_UnbindAudioStreams(&stream, 1);
}

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream)
{
    SDL_AudioDeviceID retval = 0;
    if (stream) {
        SDL_LockMutex(stream->lock);
        if (stream->bound_device) {
            retval = stream->bound_device->instance_id;
        }
        SDL_UnlockMutex(stream->lock);
    }
    return retval;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata)
{
    SDL_AudioDeviceID logdevid = SDL_OpenAudioDevice(devid, spec);
    if (!logdevid) {
        return NULL;  // error string should already be set.
    }

    SDL_LogicalAudioDevice *logdev = ObtainLogicalAudioDevice(logdevid);
    if (logdev == NULL) { // this shouldn't happen, but just in case.
        SDL_CloseAudioDevice(logdevid);
        return NULL;  // error string should already be set.
    }

    SDL_AudioDevice *physdevice = logdev->physical_device;
    SDL_assert(physdevice != NULL);

    SDL_AtomicSet(&logdev->paused, 1);   // start the device paused, to match SDL2.
    SDL_UnlockMutex(physdevice->lock);  // we don't need to hold the lock for any of this.

    const SDL_bool iscapture = physdevice->iscapture;

    SDL_AudioStream *stream = NULL;
    if (iscapture) {
        stream = SDL_CreateAudioStream(&physdevice->spec, spec);
    } else {
        stream = SDL_CreateAudioStream(spec, &physdevice->spec);
    }

    if (!stream) {
        SDL_CloseAudioDevice(logdevid);
        return NULL;  // error string should already be set.
    }
    if (SDL_BindAudioStream(logdevid, stream) == -1) {
        SDL_DestroyAudioStream(stream);
        SDL_CloseAudioDevice(logdevid);
        return NULL;  // error string should already be set.
    }

    logdev->simplified = SDL_TRUE;  // forbid further binding changes on this logical device.
    stream->simplified = SDL_TRUE;  // so we know to close the audio device when this is destroyed.

    if (callback) {
        int rc;
        if (iscapture) {
            rc = SDL_SetAudioStreamPutCallback(stream, callback, userdata);
        } else {
            rc = SDL_SetAudioStreamGetCallback(stream, callback, userdata);
        }
        SDL_assert(rc == 0);  // should only fail if stream==NULL atm.
    }

    return stream;   // ready to rock.
}

#define NUM_FORMATS 8
static const SDL_AudioFormat format_list[NUM_FORMATS][NUM_FORMATS + 1] = {
    { SDL_AUDIO_U8, SDL_AUDIO_S8, SDL_AUDIO_S16LE, SDL_AUDIO_S16BE, SDL_AUDIO_S32LE, SDL_AUDIO_S32BE, SDL_AUDIO_F32LE, SDL_AUDIO_F32BE, 0 },
    { SDL_AUDIO_S8, SDL_AUDIO_U8, SDL_AUDIO_S16LE, SDL_AUDIO_S16BE, SDL_AUDIO_S32LE, SDL_AUDIO_S32BE, SDL_AUDIO_F32LE, SDL_AUDIO_F32BE, 0 },
    { SDL_AUDIO_S16LE, SDL_AUDIO_S16BE, SDL_AUDIO_S32LE, SDL_AUDIO_S32BE, SDL_AUDIO_F32LE, SDL_AUDIO_F32BE, SDL_AUDIO_U8, SDL_AUDIO_S8, 0 },
    { SDL_AUDIO_S16BE, SDL_AUDIO_S16LE, SDL_AUDIO_S32BE, SDL_AUDIO_S32LE, SDL_AUDIO_F32BE, SDL_AUDIO_F32LE, SDL_AUDIO_U8, SDL_AUDIO_S8, 0 },
    { SDL_AUDIO_S32LE, SDL_AUDIO_S32BE, SDL_AUDIO_F32LE, SDL_AUDIO_F32BE, SDL_AUDIO_S16LE, SDL_AUDIO_S16BE, SDL_AUDIO_U8, SDL_AUDIO_S8, 0 },
    { SDL_AUDIO_S32BE, SDL_AUDIO_S32LE, SDL_AUDIO_F32BE, SDL_AUDIO_F32LE, SDL_AUDIO_S16BE, SDL_AUDIO_S16LE, SDL_AUDIO_U8, SDL_AUDIO_S8, 0 },
    { SDL_AUDIO_F32LE, SDL_AUDIO_F32BE, SDL_AUDIO_S32LE, SDL_AUDIO_S32BE, SDL_AUDIO_S16LE, SDL_AUDIO_S16BE, SDL_AUDIO_U8, SDL_AUDIO_S8, 0 },
    { SDL_AUDIO_F32BE, SDL_AUDIO_F32LE, SDL_AUDIO_S32BE, SDL_AUDIO_S32LE, SDL_AUDIO_S16BE, SDL_AUDIO_S16LE, SDL_AUDIO_U8, SDL_AUDIO_S8, 0 },
};

const SDL_AudioFormat *SDL_ClosestAudioFormats(SDL_AudioFormat format)
{
    for (int i = 0; i < NUM_FORMATS; i++) {
        if (format_list[i][0] == format) {
            return &format_list[i][0];
        }
    }
    return &format_list[0][NUM_FORMATS]; // not found; return what looks like a list with only a zero in it.
}

int SDL_GetSilenceValueForFormat(SDL_AudioFormat format)
{
    return (format == SDL_AUDIO_U8) ? 0x80 : 0x00;
}

// called internally by backends when the system default device changes.
void SDL_DefaultAudioDeviceChanged(SDL_AudioDevice *new_default_device)
{
    if (new_default_device == NULL) {  // !!! FIXME: what should we do in this case? Maybe all devices are lost, so there _isn't_ a default?
        return;  // uhoh.
    }

    const SDL_bool iscapture = new_default_device->iscapture;
    const SDL_AudioDeviceID current_devid = iscapture ? current_audio.default_capture_device_id : current_audio.default_output_device_id;

    if (new_default_device->instance_id == current_devid) {
        return;  // this is already the default.
    }

    SDL_LockMutex(new_default_device->lock);

    SDL_AudioDevice *current_default_device = ObtainPhysicalAudioDevice(current_devid);

    /* change the official default ID over while we have locks on both devices, so if something raced to open the default during
       this, it either gets the new device or is ready on the old and can be migrated. */
    if (iscapture) {
        current_audio.default_capture_device_id = new_default_device->instance_id;
    } else {
        current_audio.default_output_device_id = new_default_device->instance_id;
    }

    if (current_default_device) {
        // migrate any logical devices that were opened as a default to the new physical device...

        SDL_assert(current_default_device->iscapture == iscapture);

        // See if we have to open the new physical device, and if so, find the best audiospec for it.
        SDL_AudioSpec spec;
        SDL_bool needs_migration = SDL_FALSE;
        SDL_zero(spec);

        for (SDL_LogicalAudioDevice *logdev = current_default_device->logical_devices; logdev != NULL; logdev = logdev->next) {
            if (logdev->opened_as_default) {
                needs_migration = SDL_TRUE;
                for (SDL_AudioStream *stream = logdev->bound_streams; stream != NULL; stream = stream->next_binding) {
                    const SDL_AudioSpec *streamspec = iscapture ? &stream->dst_spec : &stream->src_spec;
                    if (SDL_AUDIO_BITSIZE(streamspec->format) > SDL_AUDIO_BITSIZE(spec.format)) {
                        spec.format = streamspec->format;
                    }
                    if (streamspec->channels > spec.channels) {
                        spec.channels = streamspec->channels;
                    }
                    if (streamspec->freq > spec.freq) {
                        spec.freq = streamspec->freq;
                    }
                }
            }
        }

        if (needs_migration) {
            if (!new_default_device->currently_opened) {  // New default physical device not been opened yet? Open at the OS level...
                if (OpenPhysicalAudioDevice(new_default_device, &spec) == -1) {
                    needs_migration = SDL_FALSE;  // uhoh, just leave everything on the old default, nothing to be done.
                }
            }
        }

        if (needs_migration) {
            const SDL_bool spec_changed = !AUDIO_SPECS_EQUAL(current_default_device->spec, new_default_device->spec);
            const SDL_bool post_fmt_event = (spec_changed && SDL_EventEnabled(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED)) ? SDL_TRUE : SDL_FALSE;
            SDL_LogicalAudioDevice *next = NULL;
            for (SDL_LogicalAudioDevice *logdev = current_default_device->logical_devices; logdev != NULL; logdev = next) {
                next = logdev->next;

                if (!logdev->opened_as_default) {
                    continue;  // not opened as a default, leave it on the current physical device.
                }

                // now migrate the logical device.
                if (logdev->next) {
                    logdev->next->prev = logdev->prev;
                }
                if (logdev->prev) {
                    logdev->prev->next = logdev->next;
                }
                if (current_default_device->logical_devices == logdev) {
                    current_default_device->logical_devices = logdev->next;
                }

                logdev->physical_device = new_default_device;
                logdev->prev = NULL;
                logdev->next = new_default_device->logical_devices;
                new_default_device->logical_devices = logdev;

                SDL_assert(SDL_AtomicGet(&current_default_device->refcount) > 1);  // we should hold at least one extra reference to this device, beyond logical devices, during this phase...
                RefPhysicalAudioDevice(new_default_device);
                UnrefPhysicalAudioDevice(current_default_device);

                // Post an event for each logical device we moved.
                if (post_fmt_event) {
                    SDL_Event event;
                    SDL_zero(event);
                    event.type = SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED;
                    event.common.timestamp = 0;
                    event.adevice.iscapture = iscapture ? 1 : 0;
                    event.adevice.which = logdev->instance_id;
                    SDL_PushEvent(&event);
                }
            }

            UpdateAudioStreamFormatsPhysical(current_default_device);
            UpdateAudioStreamFormatsPhysical(new_default_device);

            if (current_default_device->logical_devices == NULL) {   // nothing left on the current physical device, close it.
                // !!! FIXME: we _need_ to release this lock, but doing so can cause a race condition if someone opens a device while we're closing it.
                SDL_UnlockMutex(current_default_device->lock);  // can't hold the lock or the audio thread will deadlock while we WaitThread it.
                ClosePhysicalAudioDevice(current_default_device);
                SDL_LockMutex(current_default_device->lock);  // we're about to unlock this again, so make sure the locks match.
            }
        }

        SDL_UnlockMutex(current_default_device->lock);
    }

    SDL_UnlockMutex(new_default_device->lock);

    // was current device already dead and just kept around to migrate to a new default device? Now we can kill it. Aim for the brain.
    if (current_default_device && SDL_AtomicGet(&current_default_device->zombie)) {
        UnrefPhysicalAudioDevice(current_default_device);
    }
}

int SDL_AudioDeviceFormatChangedAlreadyLocked(SDL_AudioDevice *device, const SDL_AudioSpec *newspec, int new_sample_frames)
{
    const int orig_work_buffer_size = device->work_buffer_size;

    if (AUDIO_SPECS_EQUAL(device->spec, *newspec) && new_sample_frames == device->sample_frames) {
        return 0;  // we're already in that format.
    }

    SDL_copyp(&device->spec, newspec);
    UpdateAudioStreamFormatsPhysical(device);

    SDL_bool kill_device = SDL_FALSE;

    device->sample_frames = new_sample_frames;
    SDL_UpdatedAudioDeviceFormat(device);
    if (device->work_buffer && (device->work_buffer_size > orig_work_buffer_size)) {
        SDL_aligned_free(device->work_buffer);
        device->work_buffer = (Uint8 *)SDL_aligned_alloc(SDL_SIMDGetAlignment(), device->work_buffer_size);
        if (!device->work_buffer) {
            kill_device = SDL_TRUE;
        }

        if (device->postmix_buffer) {
            SDL_aligned_free(device->postmix_buffer);
            device->postmix_buffer = (float *)SDL_aligned_alloc(SDL_SIMDGetAlignment(), device->work_buffer_size);
            if (!device->postmix_buffer) {
                kill_device = SDL_TRUE;
            }
        }

        SDL_aligned_free(device->mix_buffer);
        device->mix_buffer = NULL;
        if (device->spec.format != SDL_AUDIO_F32) {
            device->mix_buffer = (Uint8 *)SDL_aligned_alloc(SDL_SIMDGetAlignment(), device->work_buffer_size);
            if (!device->mix_buffer) {
                kill_device = SDL_TRUE;
            }
        }
    }

    // Post an event for the physical device, and each logical device on this physical device.
    if (!kill_device && SDL_EventEnabled(SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED)) {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED;
        event.common.timestamp = 0;
        event.adevice.iscapture = device->iscapture ? 1 : 0;
        event.adevice.which = device->instance_id;
        SDL_PushEvent(&event);
        for (SDL_LogicalAudioDevice *logdev = device->logical_devices; logdev != NULL; logdev = logdev->next) {
            event.adevice.which = logdev->instance_id;
            SDL_PushEvent(&event);
        }
    }

    return kill_device ? -1 : 0;
}

int SDL_AudioDeviceFormatChanged(SDL_AudioDevice *device, const SDL_AudioSpec *newspec, int new_sample_frames)
{
    SDL_LockMutex(device->lock);
    const int retval = SDL_AudioDeviceFormatChangedAlreadyLocked(device, newspec, new_sample_frames);
    SDL_UnlockMutex(device->lock);
    return retval;
}

