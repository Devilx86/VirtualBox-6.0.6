/* $Id$ */
/** @file
 * Recording (with optional audio recording) code.
 *
 * This code employs a separate encoding thread per recording context
 * to keep time spent in EMT as short as possible. Each configured VM display
 * is represented by an own recording stream, which in turn has its own rendering
 * queue. Common recording data across all recording streams is kept in a
 * separate queue in the recording context to minimize data duplication and
 * multiplexing overhead in EMT.
 */

/*
 * Copyright (C) 2012-2018 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef LOG_GROUP
# undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_MAIN_DISPLAY
#include "LoggingNew.h"

#include <stdexcept>
#include <vector>

#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/path.h>
#include <iprt/semaphore.h>
#include <iprt/thread.h>
#include <iprt/time.h>

#include <VBox/err.h>
#include <VBox/com/VirtualBox.h>

#include "ConsoleImpl.h"
#include "Recording.h"
#include "RecordingInternals.h"
#include "RecordingStream.h"
#include "RecordingUtils.h"
#include "WebMWriter.h"

using namespace com;

#ifdef DEBUG_andy
/** Enables dumping audio / video data for debugging reasons. */
//# define VBOX_RECORDING_DUMP
#endif

#ifdef VBOX_RECORDING_DUMP
#pragma pack(push)
#pragma pack(1)
typedef struct
{
    uint16_t u16Magic;
    uint32_t u32Size;
    uint16_t u16Reserved1;
    uint16_t u16Reserved2;
    uint32_t u32OffBits;
} RECORDINGBMPHDR, *PRECORDINGBMPHDR;
AssertCompileSize(RECORDINGBMPHDR, 14);

typedef struct
{
    uint32_t u32Size;
    uint32_t u32Width;
    uint32_t u32Height;
    uint16_t u16Planes;
    uint16_t u16BitCount;
    uint32_t u32Compression;
    uint32_t u32SizeImage;
    uint32_t u32XPelsPerMeter;
    uint32_t u32YPelsPerMeter;
    uint32_t u32ClrUsed;
    uint32_t u32ClrImportant;
} RECORDINGBMPDIBHDR, *PRECORDINGBMPDIBHDR;
AssertCompileSize(RECORDINGBMPDIBHDR, 40);

#pragma pack(pop)
#endif /* VBOX_RECORDING_DUMP */


RecordingContext::RecordingContext(Console *a_pConsole)
    : pConsole(a_pConsole)
    , enmState(RECORDINGSTS_UNINITIALIZED) { }

RecordingContext::RecordingContext(Console *a_pConsole, const settings::RecordingSettings &a_Settings)
    : pConsole(a_pConsole)
    , enmState(RECORDINGSTS_UNINITIALIZED)
{
    int rc = RecordingContext::createInternal(a_Settings);
    if (RT_FAILURE(rc))
        throw rc;
}

RecordingContext::~RecordingContext(void)
{
    destroyInternal();
}

/**
 * Worker thread for all streams of a recording context.
 *
 * For video frames, this also does the RGB/YUV conversion and encoding.
 */
DECLCALLBACK(int) RecordingContext::threadMain(RTTHREAD hThreadSelf, void *pvUser)
{
    RecordingContext *pThis = (RecordingContext *)pvUser;

    /* Signal that we're up and rockin'. */
    RTThreadUserSignal(hThreadSelf);

    LogFunc(("Thread started\n"));

    for (;;)
    {
        int rc = RTSemEventWait(pThis->WaitEvent, RT_INDEFINITE_WAIT);
        AssertRCBreak(rc);

        Log2Func(("Processing %zu streams\n", pThis->vecStreams.size()));

        /** @todo r=andy This is inefficient -- as we already wake up this thread
         *               for every screen from Main, we here go again (on every wake up) through
         *               all screens.  */
        RecordingStreams::iterator itStream = pThis->vecStreams.begin();
        while (itStream != pThis->vecStreams.end())
        {
            RecordingStream *pStream = (*itStream);

            rc = pStream->Process(pThis->mapBlocksCommon);
            if (RT_FAILURE(rc))
                break;

            ++itStream;
        }

        if (RT_FAILURE(rc))
            LogRel(("Recording: Encoding thread failed with rc=%Rrc\n", rc));

        /* Keep going in case of errors. */

        if (ASMAtomicReadBool(&pThis->fShutdown))
        {
            LogFunc(("Thread is shutting down ...\n"));
            break;
        }

    } /* for */

    LogFunc(("Thread ended\n"));
    return VINF_SUCCESS;
}

/**
 * Notifies a recording context's encoding thread.
 *
 * @returns IPRT status code.
 */
int RecordingContext::threadNotify(void)
{
    return RTSemEventSignal(this->WaitEvent);
}

/**
 * Creates a recording context.
 *
 * @returns IPRT status code.
 * @param   a_Settings          Capture settings to use for context creation.
 */
int RecordingContext::createInternal(const settings::RecordingSettings &a_Settings)
{
    int rc = RTCritSectInit(&this->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    settings::RecordingScreenMap::const_iterator itScreen = a_Settings.mapScreens.begin();
    while (itScreen != a_Settings.mapScreens.end())
    {
        RecordingStream *pStream = NULL;
        try
        {
            pStream = new RecordingStream(this, itScreen->first /* Screen ID */, itScreen->second);
            this->vecStreams.push_back(pStream);
        }
        catch (std::bad_alloc &)
        {
            rc = VERR_NO_MEMORY;
            break;
        }

        ++itScreen;
    }

    if (RT_SUCCESS(rc))
    {
        this->tsStartMs = RTTimeMilliTS();
        this->enmState  = RECORDINGSTS_CREATED;
        this->fShutdown = false;

        /* Copy the settings to our context. */
        this->Settings  = a_Settings;

        rc = RTSemEventCreate(&this->WaitEvent);
        AssertRCReturn(rc, rc);
    }

    if (RT_FAILURE(rc))
    {
        int rc2 = destroyInternal();
        AssertRC(rc2);
    }

    return rc;
}

int RecordingContext::startInternal(void)
{
    if (this->enmState == RECORDINGSTS_STARTED)
        return VINF_SUCCESS;

    Assert(this->enmState == RECORDINGSTS_CREATED);

    int rc = RTThreadCreate(&this->Thread, RecordingContext::threadMain, (void *)this, 0,
                            RTTHREADTYPE_MAIN_WORKER, RTTHREADFLAGS_WAITABLE, "Record");

    if (RT_SUCCESS(rc)) /* Wait for the thread to start. */
        rc = RTThreadUserWait(this->Thread, 30 * RT_MS_1SEC /* 30s timeout */);

    if (RT_SUCCESS(rc))
    {
        this->enmState = RECORDINGSTS_STARTED;
    }

    return rc;
}

int RecordingContext::stopInternal(void)
{
    if (this->enmState != RECORDINGSTS_STARTED)
        return VINF_SUCCESS;

    LogThisFunc(("Shutting down thread ...\n"));

    /* Set shutdown indicator. */
    ASMAtomicWriteBool(&this->fShutdown, true);

    /* Signal the thread and wait for it to shut down. */
    int rc = threadNotify();
    if (RT_SUCCESS(rc))
        rc = RTThreadWait(this->Thread, 30 * 1000 /* 10s timeout */, NULL);

    if (RT_SUCCESS(rc))
    {
        this->enmState = RECORDINGSTS_CREATED;
    }

    LogFlowThisFunc(("%Rrc\n", rc));
    return rc;
}

/**
 * Destroys a recording context.
 */
int RecordingContext::destroyInternal(void)
{
    int rc = stopInternal();
    if (RT_FAILURE(rc))
        return rc;

    rc = RTSemEventDestroy(this->WaitEvent);
    AssertRC(rc);

    this->WaitEvent = NIL_RTSEMEVENT;

    rc = RTCritSectEnter(&this->CritSect);
    if (RT_SUCCESS(rc))
    {
        RecordingStreams::iterator it = this->vecStreams.begin();
        while (it != this->vecStreams.end())
        {
            RecordingStream *pStream = (*it);

            int rc2 = pStream->Uninit();
            if (RT_SUCCESS(rc))
                rc = rc2;

            delete pStream;
            pStream = NULL;

            this->vecStreams.erase(it);
            it = this->vecStreams.begin();

            delete pStream;
            pStream = NULL;
        }

        /* Sanity. */
        Assert(this->vecStreams.empty());
        Assert(this->mapBlocksCommon.size() == 0);

        int rc2 = RTCritSectLeave(&this->CritSect);
        AssertRC(rc2);

        RTCritSectDelete(&this->CritSect);
    }

    LogFlowThisFunc(("%Rrc\n", rc));
    return rc;
}

const settings::RecordingSettings &RecordingContext::GetConfig(void) const
{
    return this->Settings;
}

RecordingStream *RecordingContext::getStreamInternal(unsigned uScreen) const
{
    RecordingStream *pStream;

    try
    {
        pStream = this->vecStreams.at(uScreen);
    }
    catch (std::out_of_range &)
    {
        pStream = NULL;
    }

    return pStream;
}

/**
 * Retrieves a specific recording stream of a recording context.
 *
 * @returns Pointer to recording stream if found, or NULL if not found.
 * @param   uScreen             Screen number of recording stream to look up.
 */
RecordingStream *RecordingContext::GetStream(unsigned uScreen) const
{
    return getStreamInternal(uScreen);
}

size_t RecordingContext::GetStreamCount(void) const
{
    return this->vecStreams.size();
}

int RecordingContext::Create(const settings::RecordingSettings &a_Settings)
{
    return createInternal(a_Settings);
}

int RecordingContext::Destroy(void)
{
    return destroyInternal();
}

int RecordingContext::Start(void)
{
    return startInternal();
}

int RecordingContext::Stop(void)
{
    return stopInternal();
}

bool RecordingContext::IsFeatureEnabled(RecordingFeature_T enmFeature) const
{
    RecordingStreams::const_iterator itStream = this->vecStreams.begin();
    while (itStream != this->vecStreams.end())
    {
        if ((*itStream)->GetConfig().isFeatureEnabled(enmFeature))
            return true;
        ++itStream;
    }

    return false;
}

/**
 * Returns if this recording context is ready to start recording.
 *
 * @returns @c true if recording context is ready, @c false if not.
 */
bool RecordingContext::IsReady(void) const
{
    return (this->enmState >= RECORDINGSTS_CREATED);
}

/**
 * Returns if this recording context is ready to accept new recording data for a given screen.
 *
 * @returns @c true if the specified screen is ready, @c false if not.
 * @param   uScreen             Screen ID.
 * @param   uTimeStampMs        Current time stamp (in ms). Currently not being used.
 */
bool RecordingContext::IsReady(uint32_t uScreen, uint64_t uTimeStampMs) const
{
    RT_NOREF(uTimeStampMs);

    if (this->enmState != RECORDINGSTS_STARTED)
        return false;

    bool fIsReady = false;

    const RecordingStream *pStream = GetStream(uScreen);
    if (pStream)
        fIsReady = pStream->IsReady();

    /* Note: Do not check for other constraints like the video FPS rate here,
     *       as this check then also would affect other (non-FPS related) stuff
     *       like audio data. */

    return fIsReady;
}

/**
 * Returns whether a given recording context has been started or not.
 *
 * @returns true if active, false if not.
 */
bool RecordingContext::IsStarted(void) const
{
    return (this->enmState == RECORDINGSTS_STARTED);
}

/**
 * Checks if a specified limit for recording has been reached.
 *
 * @returns true if any limit has been reached.
 * @param   uScreen             Screen ID.
 * @param   tsNowMs             Current time stamp (in ms).
 */
bool RecordingContext::IsLimitReached(uint32_t uScreen, uint64_t tsNowMs) const
{
    const RecordingStream *pStream = GetStream(uScreen);
    if (   !pStream
        || pStream->IsLimitReached(tsNowMs))
    {
        return true;
    }

    return false;
}

/**
 * Sends an audio frame to the video encoding thread.
 *
 * @thread  EMT
 *
 * @returns IPRT status code.
 * @param   pvData              Audio frame data to send.
 * @param   cbData              Size (in bytes) of (encoded) audio frame data.
 * @param   uTimeStampMs        Time stamp (in ms) of audio playback.
 */
int RecordingContext::SendAudioFrame(const void *pvData, size_t cbData, uint64_t uTimeStampMs)
{
#ifdef VBOX_WITH_AUDIO_RECORDING
    AssertPtrReturn(pvData, VERR_INVALID_POINTER);
    AssertReturn(cbData, VERR_INVALID_PARAMETER);

    /* To save time spent in EMT, do the required audio multiplexing in the encoding thread.
     *
     * The multiplexing is needed to supply all recorded (enabled) screens with the same
     * audio data at the same given point in time.
     */
    PRECORDINGBLOCK pBlock = (PRECORDINGBLOCK)RTMemAlloc(sizeof(RECORDINGBLOCK));
    AssertPtrReturn(pBlock, VERR_NO_MEMORY);
    pBlock->enmType = RECORDINGBLOCKTYPE_AUDIO;

    PRECORDINGAUDIOFRAME pFrame = (PRECORDINGAUDIOFRAME)RTMemAlloc(sizeof(RECORDINGAUDIOFRAME));
    AssertPtrReturn(pFrame, VERR_NO_MEMORY);

    pFrame->pvBuf = (uint8_t *)RTMemAlloc(cbData);
    AssertPtrReturn(pFrame->pvBuf, VERR_NO_MEMORY);
    pFrame->cbBuf = cbData;

    memcpy(pFrame->pvBuf, pvData, cbData);

    pBlock->pvData       = pFrame;
    pBlock->cbData       = sizeof(RECORDINGAUDIOFRAME) + cbData;
    pBlock->cRefs        = (uint16_t)this->vecStreams.size(); /* All streams need the same audio data. */
    pBlock->uTimeStampMs = uTimeStampMs;

    int rc = RTCritSectEnter(&this->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    try
    {
        RecordingBlockMap::iterator itBlocks = this->mapBlocksCommon.find(uTimeStampMs);
        if (itBlocks == this->mapBlocksCommon.end())
        {
            RecordingBlocks *pRecordingBlocks = new RecordingBlocks();
            pRecordingBlocks->List.push_back(pBlock);

            this->mapBlocksCommon.insert(std::make_pair(uTimeStampMs, pRecordingBlocks));
        }
        else
            itBlocks->second->List.push_back(pBlock);
    }
    catch (const std::exception &ex)
    {
        RT_NOREF(ex);
        rc = VERR_NO_MEMORY;
    }

    int rc2 = RTCritSectLeave(&this->CritSect);
    AssertRC(rc2);

    if (RT_SUCCESS(rc))
        rc = threadNotify();

    return rc;
#else
    RT_NOREF(pCtx, pvData, cbData, uTimeStampMs);
    return VINF_SUCCESS;
#endif
}

/**
 * Copies a source video frame to the intermediate RGB buffer.
 * This function is executed only once per time.
 *
 * @thread  EMT
 *
 * @returns IPRT status code.
 * @param   uScreen            Screen number to send video frame to.
 * @param   x                  Starting x coordinate of the video frame.
 * @param   y                  Starting y coordinate of the video frame.
 * @param   uPixelFormat       Pixel format.
 * @param   uBPP               Bits Per Pixel (BPP).
 * @param   uBytesPerLine      Bytes per scanline.
 * @param   uSrcWidth          Width of the video frame.
 * @param   uSrcHeight         Height of the video frame.
 * @param   puSrcData          Pointer to video frame data.
 * @param   uTimeStampMs       Time stamp (in ms).
 */
int RecordingContext::SendVideoFrame(uint32_t uScreen, uint32_t x, uint32_t y,
                                   uint32_t uPixelFormat, uint32_t uBPP, uint32_t uBytesPerLine,
                                   uint32_t uSrcWidth, uint32_t uSrcHeight, uint8_t *puSrcData,
                                   uint64_t uTimeStampMs)
{
    AssertReturn(uSrcWidth,  VERR_INVALID_PARAMETER);
    AssertReturn(uSrcHeight, VERR_INVALID_PARAMETER);
    AssertReturn(puSrcData,  VERR_INVALID_POINTER);

    int rc = RTCritSectEnter(&this->CritSect);
    AssertRC(rc);

    RecordingStream *pStream = GetStream(uScreen);
    if (!pStream)
    {
        rc = RTCritSectLeave(&this->CritSect);
        AssertRC(rc);

        return VERR_NOT_FOUND;
    }

    rc = pStream->SendVideoFrame(x, y, uPixelFormat, uBPP, uBytesPerLine, uSrcWidth, uSrcHeight, puSrcData, uTimeStampMs);

    int rc2 = RTCritSectLeave(&this->CritSect);
    AssertRC(rc2);

    if (   RT_SUCCESS(rc)
        && rc != VINF_TRY_AGAIN) /* Only signal the thread if operation was successful. */
    {
        threadNotify();
    }

    return rc;
}

