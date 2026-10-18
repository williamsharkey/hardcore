/* Camera capture on Windows with Media Foundation, loaded at run time so a
   Windows without it (the N editions) still starts. Preview frames arrive as
   BGRA on a worker thread; a recording is camera.mp4-style H.264 plus AAC
   from the microphone, written to Videos\Hardcore. The microphone is opened
   only while recording, so the privacy light for it never shows otherwise. */
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#define WM_APP_CAMERA (WM_APP + 12)
enum { CAM_IDLE, CAM_PREPARING, CAM_READY, CAM_RECORDING, CAM_PAUSED, CAM_FINISHING, CAM_FAILED };

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *PFN_MFEnumDeviceSources)(IMFAttributes *, IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromMediaSource)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **);
typedef HRESULT (WINAPI *PFN_MFCreateSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
static struct {
    PFN_MFStartup startup;
    PFN_MFCreateAttributes attributes;
    PFN_MFCreateMediaType media_type;
    PFN_MFEnumDeviceSources enum_sources;
    PFN_MFCreateSourceReaderFromMediaSource reader;
    PFN_MFCreateSinkWriterFromURL writer;
    int tried, ok;
} g_mf;
static int mf_load(void) {
    if (!g_mf.tried) {
        HMODULE plat = LoadLibraryW(L"mfplat.dll"), mf = LoadLibraryW(L"mf.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
        g_mf.tried = 1;
        if (plat && mf && rw) {
            g_mf.startup = (PFN_MFStartup)(void *)GetProcAddress(plat, "MFStartup");
            g_mf.attributes = (PFN_MFCreateAttributes)(void *)GetProcAddress(plat, "MFCreateAttributes");
            g_mf.media_type = (PFN_MFCreateMediaType)(void *)GetProcAddress(plat, "MFCreateMediaType");
            g_mf.enum_sources = (PFN_MFEnumDeviceSources)(void *)GetProcAddress(mf, "MFEnumDeviceSources");
            g_mf.reader = (PFN_MFCreateSourceReaderFromMediaSource)(void *)GetProcAddress(rw, "MFCreateSourceReaderFromMediaSource");
            g_mf.writer = (PFN_MFCreateSinkWriterFromURL)(void *)GetProcAddress(rw, "MFCreateSinkWriterFromURL");
            g_mf.ok = g_mf.startup && g_mf.attributes && g_mf.media_type && g_mf.enum_sources && g_mf.reader && g_mf.writer &&
                      SUCCEEDED(g_mf.startup(MF_VERSION, MFSTARTUP_FULL));
        }
    }
    return g_mf.ok;
}

static struct {
    CRITICAL_SECTION lock;
    int lock_ready;
    volatile LONG state, quit, stop, discard, paused;
    HANDLE video_thread, record_thread;
    char name[128], error[160], status[300];
    int has_mic;
    /* the newest preview frame, top-down BGRA */
    unsigned char *frame;
    int fw, fh;
    volatile LONG frame_seq;
    UINT32 fps_num, fps_den;
    IMFMediaType *video_type;       /* the reader's RGB32 output: the writer's input */
    /* recording */
    IMFSinkWriter *writer;
    DWORD vstream, astream;
    int writing_audio;
    LARGE_INTEGER freq, began;
    LONGLONG paused_total, pause_began, last_video, audio_cursor;
    wchar_t path[MAX_PATH];
    DWORD last_paint;
} g_cam;

static void cam_post(void) { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_CAMERA, 0, 0); }
static void cam_lock(void) {
    if (!g_cam.lock_ready) { InitializeCriticalSection(&g_cam.lock); g_cam.lock_ready = 1; }
    EnterCriticalSection(&g_cam.lock);
}
static void cam_unlock(void) { LeaveCriticalSection(&g_cam.lock); }
static void cam_fail(const char *message) {
    cam_lock();
    snprintf(g_cam.error, sizeof(g_cam.error), "%s", message);
    cam_unlock();
    InterlockedExchange(&g_cam.state, CAM_FAILED);
    cam_post();
}
static int cam_state(void) { return (int)g_cam.state; }
static LONGLONG cam_now(void) {   /* 100 ns since recording began, pauses left out */
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (LONGLONG)((double)(t.QuadPart - g_cam.began.QuadPart) * 1e7 / (double)g_cam.freq.QuadPart);
}
static double cam_elapsed(void) {
    int s = cam_state();
    if (s != CAM_RECORDING && s != CAM_PAUSED && s != CAM_FINISHING) return 0;
    LONGLONG now = s == CAM_PAUSED ? g_cam.pause_began : cam_now();
    return (double)(now - g_cam.paused_total) / 1e7;
}

/* The first device of a kind, activated. */
static IMFMediaSource *cam_device(const GUID *kind, char *name, size_t cap) {
    IMFAttributes *a = NULL;
    IMFActivate **devices = NULL;
    UINT32 count = 0;
    IMFMediaSource *source = NULL;
    if (FAILED(g_mf.attributes(&a, 1))) return NULL;
    IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, kind);
    if (SUCCEEDED(g_mf.enum_sources(a, &devices, &count)) && count) {
        if (name) {
            WCHAR *friendly = NULL;
            UINT32 len;
            if (SUCCEEDED(IMFActivate_GetAllocatedString(devices[0], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &len))) {
                WideCharToMultiByte(CP_UTF8, 0, friendly, -1, name, (int)cap, NULL, NULL);
                CoTaskMemFree(friendly);
            }
        }
        if (FAILED(IMFActivate_ActivateObject(devices[0], &IID_IMFMediaSource, (void **)&source))) source = NULL;
    }
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devices[i]);
    CoTaskMemFree(devices);
    IMFAttributes_Release(a);
    return source;
}

static int cam_device_count(const GUID *kind) {
    IMFAttributes *a = NULL;
    IMFActivate **devices = NULL;
    UINT32 count = 0;
    if (FAILED(g_mf.attributes(&a, 1))) return 0;
    IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, kind);
    if (FAILED(g_mf.enum_sources(a, &devices, &count))) count = 0;
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(devices[i]);
    CoTaskMemFree(devices);
    IMFAttributes_Release(a);
    return (int)count;
}

/* Of the camera's own modes, the one nearest 1280×720 at 24 fps or more. */
static void cam_pick_mode(IMFSourceReader *r) {
    IMFMediaType *t, *best = NULL;
    long best_score = LONG_MAX;
    for (DWORD i = 0; SUCCEEDED(IMFSourceReader_GetNativeMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t)); i++) {
        UINT64 size = 0, rate = 0;
        IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &size);
        IMFMediaType_GetUINT64(t, &MF_MT_FRAME_RATE, &rate);
        long w = (long)(size >> 32), h = (long)(size & 0xFFFFFFFF);
        double fps = (rate & 0xFFFFFFFF) ? (double)(rate >> 32) / (double)(rate & 0xFFFFFFFF) : 0;
        long score = labs(w - 1280) + labs(h - 720) + (fps < 23.5 ? 100000 : 0) + (w > 1920 ? 50000 : 0);
        if (score < best_score) {
            if (best) IMFMediaType_Release(best);
            best = t; best_score = score;
        } else IMFMediaType_Release(t);
    }
    if (best) {
        IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, best);
        IMFMediaType_Release(best);
    }
}

static DWORD WINAPI cam_video_thread(LPVOID unused) {
    IMFMediaSource *source = NULL;
    IMFSourceReader *reader = NULL;
    IMFAttributes *a = NULL;
    IMFMediaType *want = NULL, *got = NULL;
    INT32 stride = 0;
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!mf_load()) { cam_fail("Media Foundation isn't installed on this Windows"); goto done; }
    source = cam_device(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, g_cam.name, sizeof(g_cam.name));
    if (!source) {
        cam_fail(g_cam.name[0] ? "Camera unavailable: allow desktop apps in Settings › Privacy › Camera" : "No camera found");
        goto done;
    }
    g_cam.has_mic = cam_device_count(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID) > 0;
    g_mf.attributes(&a, 2);
    IMFAttributes_SetUINT32(a, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(g_mf.reader(source, a, &reader))) { cam_fail("Camera unavailable"); goto done; }
    cam_pick_mode(reader);
    g_mf.media_type(&want);
    IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want)) ||
        FAILED(IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got))) {
        cam_fail("Camera format not supported"); goto done;
    }
    {
        UINT64 size = 0, rate = 0;
        IMFMediaType_GetUINT64(got, &MF_MT_FRAME_SIZE, &size);
        if (FAILED(IMFMediaType_GetUINT64(got, &MF_MT_FRAME_RATE, &rate)) || !(rate & 0xFFFFFFFF)) rate = ((UINT64)30 << 32) | 1;
        cam_lock();
        g_cam.fw = (int)(size >> 32); g_cam.fh = (int)(size & 0xFFFFFFFF);
        g_cam.fps_num = (UINT32)(rate >> 32); g_cam.fps_den = (UINT32)(rate & 0xFFFFFFFF);
        free(g_cam.frame);
        g_cam.frame = (unsigned char *)calloc((size_t)g_cam.fw * g_cam.fh, 4);
        g_cam.video_type = got; IMFMediaType_AddRef(got);
        cam_unlock();
        if (FAILED(IMFMediaType_GetUINT32(got, &MF_MT_DEFAULT_STRIDE, (UINT32 *)&stride))) stride = g_cam.fw * 4;
    }
    if (!g_cam.frame || g_cam.fw <= 0 || g_cam.fh <= 0) { cam_fail("Camera format not supported"); goto done; }
    InterlockedExchange(&g_cam.state, CAM_READY);
    cam_post();
    while (!g_cam.quit) {
        DWORD stream, flags = 0;
        LONGLONG stamp;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &stamp, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            if (sample) IMFSample_Release(sample);
            if (!g_cam.quit) cam_fail("Camera stopped");
            break;
        }
        if (!sample) continue;
        IMFMediaBuffer *buf = NULL;
        BYTE *bytes;
        DWORD len;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) && SUCCEEDED(IMFMediaBuffer_Lock(buf, &bytes, NULL, &len))) {
            int row = g_cam.fw * 4, abs_stride = stride < 0 ? -stride : stride;
            if (abs_stride >= row && len >= (DWORD)(abs_stride * g_cam.fh)) {
                cam_lock();
                for (int y = 0; y < g_cam.fh; y++) {
                    const BYTE *src = bytes + (size_t)(stride < 0 ? g_cam.fh - 1 - y : y) * abs_stride;
                    memcpy(g_cam.frame + (size_t)y * row, src, (size_t)row);
                }
                cam_unlock();
                InterlockedIncrement(&g_cam.frame_seq);
            }
            IMFMediaBuffer_Unlock(buf);
        }
        if (buf) IMFMediaBuffer_Release(buf);
        /* the recording: its own clock, so camera and microphone line up */
        cam_lock();
        if (g_cam.writer && g_cam.state == CAM_RECORDING) {
            LONGLONG t = cam_now() - g_cam.paused_total;
            if (t > g_cam.last_video) {
                IMFSample_SetSampleTime(sample, t);
                IMFSample_SetSampleDuration(sample, (LONGLONG)(1e7 * g_cam.fps_den / g_cam.fps_num));
                if (FAILED(IMFSinkWriter_WriteSample(g_cam.writer, g_cam.vstream, sample)))
                    snprintf(g_cam.status, sizeof(g_cam.status), "Recording trouble: a frame could not be written");
                g_cam.last_video = t;
            }
        }
        cam_unlock();
        IMFSample_Release(sample);
        DWORD now = GetTickCount();
        if (now - g_cam.last_paint >= 15) { g_cam.last_paint = now; InvalidateRect(g_hwnd, NULL, FALSE); }
    }
done:
    if (reader) IMFSourceReader_Release(reader);
    if (got) IMFMediaType_Release(got);
    if (want) IMFMediaType_Release(want);
    if (a) IMFAttributes_Release(a);
    if (source) { IMFMediaSource_Shutdown(source); IMFMediaSource_Release(source); }
    CoUninitialize();
    return 0;
}

static void cam_videos_dir(wchar_t *out, int cap) {
    wchar_t *videos = NULL;
    static const GUID FOLDERID_Videos_ = {0x18989B1D, 0x99B5, 0x455B, {0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC}};
    typedef HRESULT (WINAPI *PFN_KnownFolder)(const GUID *, DWORD, HANDLE, PWSTR *);
    PFN_KnownFolder known = (PFN_KnownFolder)(void *)GetProcAddress(LoadLibraryW(L"shell32.dll"), "SHGetKnownFolderPath");
    if (known && SUCCEEDED(known(&FOLDERID_Videos_, 0, NULL, &videos))) {
        _snwprintf(out, cap, L"%ls\\Hardcore", videos);
        CoTaskMemFree(videos);
    } else _snwprintf(out, cap, L"%ls\\Videos\\Hardcore", _wgetenv(L"USERPROFILE") ? _wgetenv(L"USERPROFILE") : L"C:");
    out[cap - 1] = 0;
}

/* The writer's video stream: H.264 at the camera's size and rate. */
static int cam_add_video(IMFSinkWriter *w) {
    IMFMediaType *out = NULL;
    UINT64 size, rate;
    int ok = 0;
    IMFMediaType_GetUINT64(g_cam.video_type, &MF_MT_FRAME_SIZE, &size);
    rate = ((UINT64)g_cam.fps_num << 32) | g_cam.fps_den;
    if (SUCCEEDED(g_mf.media_type(&out))) {
        UINT32 pixels = (UINT32)(size >> 32) * (UINT32)(size & 0xFFFFFFFF);
        IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
        IMFMediaType_SetUINT32(out, &MF_MT_AVG_BITRATE, pixels * 6 > 2000000 ? pixels * 6 : 2000000);
        IMFMediaType_SetUINT32(out, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        IMFMediaType_SetUINT64(out, &MF_MT_FRAME_SIZE, size);
        IMFMediaType_SetUINT64(out, &MF_MT_FRAME_RATE, rate);
        IMFMediaType_SetUINT64(out, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        ok = SUCCEEDED(IMFSinkWriter_AddStream(w, out, &g_cam.vstream)) &&
             SUCCEEDED(IMFSinkWriter_SetInputMediaType(w, g_cam.vstream, g_cam.video_type, NULL));
        IMFMediaType_Release(out);
    }
    return ok;
}
/* And the microphone: 16-bit PCM in, AAC out. */
static IMFSourceReader *cam_open_mic(IMFMediaSource **source_out, IMFMediaType **pcm_out) {
    IMFMediaSource *source = cam_device(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID, NULL, 0);
    IMFSourceReader *reader = NULL;
    IMFMediaType *want = NULL, *got = NULL;
    if (!source) return NULL;
    if (SUCCEEDED(g_mf.reader(source, NULL, &reader)) && SUCCEEDED(g_mf.media_type(&want))) {
        IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(want, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(want, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        IMFMediaType_SetUINT32(want, &MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want)) &&
            SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) {
            IMFMediaType_Release(want);
            *source_out = source; *pcm_out = got;
            return reader;
        }
    }
    if (want) IMFMediaType_Release(want);
    if (reader) IMFSourceReader_Release(reader);
    IMFMediaSource_Shutdown(source);
    IMFMediaSource_Release(source);
    return NULL;
}
static int cam_add_audio(IMFSinkWriter *w, IMFMediaType *pcm) {
    IMFMediaType *out = NULL;
    UINT32 rate = 48000, channels = 2;
    int ok = 0;
    IMFMediaType_GetUINT32(pcm, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(pcm, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if ((rate != 44100 && rate != 48000) || (channels != 1 && channels != 2)) return 0;   /* what the AAC encoder takes */
    if (SUCCEEDED(g_mf.media_type(&out))) {
        IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_NUM_CHANNELS, channels);
        IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        ok = SUCCEEDED(IMFSinkWriter_AddStream(w, out, &g_cam.astream)) &&
             SUCCEEDED(IMFSinkWriter_SetInputMediaType(w, g_cam.astream, pcm, NULL));
        IMFMediaType_Release(out);
    }
    return ok;
}

/* One recording, start to finish: open the microphone, write, finalize. */
static DWORD WINAPI cam_record_thread(LPVOID unused) {
    IMFMediaSource *mic_source = NULL;
    IMFMediaType *pcm = NULL;
    IMFSourceReader *mic = NULL;
    IMFSinkWriter *w = NULL;
    IMFAttributes *a = NULL;
    wchar_t dir[MAX_PATH];
    char saved[MAX_PATH * 3];
    SYSTEMTIME st;
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    cam_videos_dir(dir, MAX_PATH);
    SHCreateDirectoryExW(NULL, dir, NULL);
    GetLocalTime(&st);
    _snwprintf(g_cam.path, MAX_PATH, L"%ls\\%04d-%02d-%02d %02d.%02d.%02d.mp4", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_cam.path[MAX_PATH - 1] = 0;
    mic = cam_open_mic(&mic_source, &pcm);
    g_mf.attributes(&a, 1);
    IMFAttributes_SetUINT32(a, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (FAILED(g_mf.writer(g_cam.path, NULL, a, &w)) || !cam_add_video(w)) {
        if (w) { IMFSinkWriter_Release(w); w = NULL; }
        DeleteFileW(g_cam.path);
        cam_lock();
        snprintf(g_cam.status, sizeof(g_cam.status), "Could not start a recording (no H.264 encoder?)");
        cam_unlock();
        InterlockedExchange(&g_cam.state, CAM_READY);
        goto done;
    }
    g_cam.writing_audio = mic && cam_add_audio(w, pcm);
    if (FAILED(IMFSinkWriter_BeginWriting(w))) {
        IMFSinkWriter_Release(w); w = NULL;
        DeleteFileW(g_cam.path);
        cam_lock();
        snprintf(g_cam.status, sizeof(g_cam.status), "Could not start a recording");
        cam_unlock();
        InterlockedExchange(&g_cam.state, CAM_READY);
        goto done;
    }
    cam_lock();
    QueryPerformanceFrequency(&g_cam.freq);
    QueryPerformanceCounter(&g_cam.began);
    g_cam.paused_total = g_cam.last_video = g_cam.audio_cursor = 0;
    g_cam.writer = w;
    g_cam.status[0] = 0;
    InterlockedExchange(&g_cam.state, CAM_RECORDING);
    cam_unlock();
    cam_post();
    while (!g_cam.stop && !g_cam.quit && cam_state() != CAM_FAILED) {
        if (!g_cam.writing_audio) { Sleep(50); continue; }
        DWORD stream, flags = 0;
        LONGLONG stamp, duration = 0;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(mic, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream, &flags, &stamp, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            if (sample) IMFSample_Release(sample);
            g_cam.writing_audio = 0;
            continue;
        }
        if (!sample) continue;
        cam_lock();
        if (g_cam.state == CAM_RECORDING) {
            /* back to back, but never behind the clock after a pause */
            LONGLONG now = cam_now() - g_cam.paused_total;
            IMFSample_GetSampleDuration(sample, &duration);
            if (g_cam.audio_cursor < now - 2000000) g_cam.audio_cursor = now;
            IMFSample_SetSampleTime(sample, g_cam.audio_cursor);
            IMFSinkWriter_WriteSample(w, g_cam.astream, sample);
            g_cam.audio_cursor += duration;
        }
        cam_unlock();
        IMFSample_Release(sample);
    }
    InterlockedExchange(&g_cam.state, CAM_FINISHING);
    cam_post();
    cam_lock();
    g_cam.writer = NULL;
    cam_unlock();
    {
        int discard = g_cam.discard, ok = SUCCEEDED(IMFSinkWriter_Finalize(w));
        IMFSinkWriter_Release(w);
        w = NULL;
        WideCharToMultiByte(CP_UTF8, 0, wcsrchr(g_cam.path, L'\\') + 1, -1, saved, sizeof(saved), NULL, NULL);
        cam_lock();
        if (discard) { DeleteFileW(g_cam.path); snprintf(g_cam.status, sizeof(g_cam.status), "Recording discarded"); }
        else if (ok) snprintf(g_cam.status, sizeof(g_cam.status), "Saved %s", saved);
        else snprintf(g_cam.status, sizeof(g_cam.status), "The recording could not be finished");
        cam_unlock();
    }
    if (cam_state() != CAM_FAILED) InterlockedExchange(&g_cam.state, g_cam.quit ? CAM_IDLE : CAM_READY);
done:
    if (a) IMFAttributes_Release(a);
    if (pcm) IMFMediaType_Release(pcm);
    if (mic) IMFSourceReader_Release(mic);
    if (mic_source) { IMFMediaSource_Shutdown(mic_source); IMFMediaSource_Release(mic_source); }
    InterlockedExchange(&g_cam.stop, 0);
    InterlockedExchange(&g_cam.discard, 0);
    CoUninitialize();
    cam_post();
    return 0;
}

static void cam_prepare(void) {
    int s = cam_state();
    if (s != CAM_IDLE && s != CAM_FAILED) return;
    cam_lock();
    g_cam.error[0] = g_cam.status[0] = 0;
    cam_unlock();
    if (g_cam.video_thread) { WaitForSingleObject(g_cam.video_thread, 3000); CloseHandle(g_cam.video_thread); }
    InterlockedExchange(&g_cam.quit, 0);
    InterlockedExchange(&g_cam.state, CAM_PREPARING);
    g_cam.video_thread = CreateThread(NULL, 0, cam_video_thread, NULL, 0, NULL);
    if (!g_cam.video_thread) cam_fail("Camera unavailable");
}
static int cam_recording(void) { int s = cam_state(); return s == CAM_RECORDING || s == CAM_PAUSED || s == CAM_FINISHING; }
static void cam_record_start(void) {
    if (cam_state() != CAM_READY || g_cam.record_thread) return;
    InterlockedExchange(&g_cam.stop, 0);
    InterlockedExchange(&g_cam.discard, 0);
    g_cam.record_thread = CreateThread(NULL, 0, cam_record_thread, NULL, 0, NULL);
}
static void cam_record_pause(void) {
    cam_lock();
    if (g_cam.state == CAM_RECORDING) { g_cam.pause_began = cam_now(); InterlockedExchange(&g_cam.state, CAM_PAUSED); }
    else if (g_cam.state == CAM_PAUSED) { g_cam.paused_total += cam_now() - g_cam.pause_began; InterlockedExchange(&g_cam.state, CAM_RECORDING); }
    cam_unlock();
}
static void cam_record_stop(int discard) {
    if (!cam_recording()) return;
    cam_lock();
    if (g_cam.state == CAM_PAUSED) { g_cam.paused_total += cam_now() - g_cam.pause_began; InterlockedExchange(&g_cam.state, CAM_RECORDING); }
    cam_unlock();
    InterlockedExchange(&g_cam.discard, discard);
    InterlockedExchange(&g_cam.stop, 1);
}
/* The camera off: the preview thread ends; a recording finishes saving first. */
static void cam_shutdown(void) {
    if (cam_recording()) return;
    InterlockedExchange(&g_cam.quit, 1);
    if (g_cam.video_thread) { WaitForSingleObject(g_cam.video_thread, 3000); CloseHandle(g_cam.video_thread); g_cam.video_thread = NULL; }
    if (g_cam.record_thread) { WaitForSingleObject(g_cam.record_thread, 10000); CloseHandle(g_cam.record_thread); g_cam.record_thread = NULL; }
    cam_lock();
    if (g_cam.video_type) { IMFMediaType_Release(g_cam.video_type); g_cam.video_type = NULL; }
    cam_unlock();
    InterlockedExchange(&g_cam.state, CAM_IDLE);
}
/* WM_APP_CAMERA: a thread changed state. Reap a finished recording thread. */
static void cam_reap(void) {
    if (g_cam.record_thread && WaitForSingleObject(g_cam.record_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_cam.record_thread);
        g_cam.record_thread = NULL;
    }
}

/* ── pictures drawn into the frame (camera preview, Chrome pages): one pixel shader ── */
static const char IMG_HLSL[] =
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(VO i) : SV_Target { return float4(t.SampleLevel(s, i.uv, 0).rgb, i.c.a); }\n";
static struct { ID3D11Device *dev; ID3D11PixelShader *ps; ID3D11SamplerState *samp; } g_imgpipe;
static int imgpipe_ready(void) {
    if (g_imgpipe.dev != g_dev) {
        if (g_imgpipe.samp) ID3D11SamplerState_Release(g_imgpipe.samp);
        if (g_imgpipe.ps) ID3D11PixelShader_Release(g_imgpipe.ps);
        memset(&g_imgpipe, 0, sizeof(g_imgpipe));
    }
    if (!g_dev) return 0;
    if (!g_imgpipe.ps) {
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
        ID3DBlob *b = NULL;
        D3D11_SAMPLER_DESC sd;
        if (!compile || FAILED(compile(IMG_HLSL, sizeof(IMG_HLSL) - 1, NULL, NULL, NULL, "ps", "ps_4_0", 0, 0, &b, NULL))) return 0;
        ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_imgpipe.ps);
        ID3D10Blob_Release(b);
        memset(&sd, 0, sizeof(sd));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(g_dev, &sd, &g_imgpipe.samp);
        g_imgpipe.dev = g_dev;
    }
    return g_imgpipe.ps && g_imgpipe.samp;
}
/* A texture into (x0,y0)-(x1,y1); u runs u0..u1 and v 0..v1, in 4096ths. */
static void img_draw(ID3D11ShaderResourceView *srv, int x0, int y0, int x1, int y1, int u0, int u1, int v1) {
    if (!srv || !imgpipe_ready()) return;
    r_flush();
    g_rgba = 0xFF000000u;
    r_quad_uv((float)x0, (float)y0, (float)x1, (float)y1, u0, 0, u1, v1);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_imgpipe.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_imgpipe.samp);
    r_flush();
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp);
}

/* ── the camera's texture, refreshed from the newest frame ── */
static struct {
    ID3D11Device *dev;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int w, h;
    LONG seq;
} g_camtex;
static void camtex_free(void) {
    if (g_camtex.srv) ID3D11ShaderResourceView_Release(g_camtex.srv);
    if (g_camtex.tex) ID3D11Texture2D_Release(g_camtex.tex);
    memset(&g_camtex, 0, sizeof(g_camtex));
}
static int camtex_ready(void) {
    if (g_camtex.dev != g_dev) camtex_free();
    if (!g_dev || !imgpipe_ready()) return 0;
    g_camtex.dev = g_dev;
    cam_lock();
    int w = g_cam.fw, h = g_cam.fh;
    if (g_cam.frame && w > 0 && h > 0 && (w != g_camtex.w || h != g_camtex.h || !g_camtex.tex)) {
        D3D11_TEXTURE2D_DESC td;
        if (g_camtex.srv) { ID3D11ShaderResourceView_Release(g_camtex.srv); g_camtex.srv = NULL; }
        if (g_camtex.tex) { ID3D11Texture2D_Release(g_camtex.tex); g_camtex.tex = NULL; }
        memset(&td, 0, sizeof(td));
        td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_camtex.tex)))
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_camtex.tex, NULL, &g_camtex.srv);
        g_camtex.w = w; g_camtex.h = h; g_camtex.seq = -1;
    }
    if (g_camtex.tex && g_camtex.seq != g_cam.frame_seq) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_camtex.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < h; y++) memcpy((unsigned char *)m.pData + (size_t)y * m.RowPitch, g_cam.frame + (size_t)y * w * 4, (size_t)w * 4);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_camtex.tex, 0);
        }
        g_camtex.seq = g_cam.frame_seq;
    }
    cam_unlock();
    return g_camtex.srv != NULL && g_cam.frame_seq > 0;
}
/* The newest frame, mirrored like a mirror, fitted inside the rectangle. */
static int cam_draw_preview(int x0, int y0, int x1, int y1) {
    if (!camtex_ready() || x1 <= x0 || y1 <= y0) return 0;
    double scale = fmin((double)(x1 - x0) / g_camtex.w, (double)(y1 - y0) / g_camtex.h);
    int w = (int)(g_camtex.w * scale), h = (int)(g_camtex.h * scale);
    int left = x0 + (x1 - x0 - w) / 2, top = y0 + (y1 - y0 - h) / 2;
    img_draw(g_camtex.srv, left, top, left + w, top + h, 4096, 0, 4096);
    return 1;
}
