/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** \brief libsndfile integration */

#include "sles_allinclusive.h"

#ifdef USE_SNDFILE


/** \brief Called by SndFile.c:audioPlayerTransportUpdate after a play state change or seek,
 *  and by IOutputMixExt::FillBuffer after each buffer is consumed.
 */

void SndFile_Callback(SLBufferQueueItf caller, void *pContext)
{
    CAudioPlayer *thisAP = (CAudioPlayer *) pContext;
    object_lock_peek(&thisAP->mObject);
    SLuint32 state = thisAP->mPlay.mState;
    object_unlock_peek(&thisAP->mObject);
    // FIXME should not muck around directly at this low level
    if (SL_PLAYSTATE_PLAYING != state) {
        return;
    }
    struct SndFile *this = &thisAP->mSndFile;
    SLresult result;
    pthread_mutex_lock(&this->mMutex);
    if (this->mEOF) {
        pthread_mutex_unlock(&this->mMutex);
        return;
    }
    short *pBuffer = &this->mBuffer[this->mWhich * SndFile_BUFSIZE];
    if (++this->mWhich >= SndFile_NUMBUFS) {
        this->mWhich = 0;
    }
    sf_count_t count;
    count = sf_read_short(this->mSNDFILE, pBuffer, (sf_count_t) SndFile_BUFSIZE);
    pthread_mutex_unlock(&this->mMutex);
    bool headAtNewPos = false;
    object_lock_exclusive(&thisAP->mObject);
    slPlayCallback callback = thisAP->mPlay.mCallback;
    void *context = thisAP->mPlay.mContext;
    // make a copy of sample rate so we are absolutely sure we will not divide by zero
    SLuint32 sampleRateMilliHz = thisAP->mSampleRateMilliHz;
    if (0 != sampleRateMilliHz) {
        // this will overflow after 49 days, but no fix possible as it's part of the API
        thisAP->mPlay.mPosition = (SLuint32) (((long long) thisAP->mPlay.mFramesSinceLastSeek *
            1000000LL) / sampleRateMilliHz) + thisAP->mPlay.mLastSeekPosition;
        // make a good faith effort for the mean time between "head at new position" callbacks to
        // occur at the requested update period, but there will be jitter
        SLuint32 frameUpdatePeriod = thisAP->mPlay.mFrameUpdatePeriod;
        if ((0 != frameUpdatePeriod) &&
            (thisAP->mPlay.mFramesSincePositionUpdate >= frameUpdatePeriod) &&
            (SL_PLAYEVENT_HEADATNEWPOS & thisAP->mPlay.mEventFlags)) {
            // if we overrun a requested update period, then reset the clock modulo the
            // update period so that it appears to the application as one or more lost callbacks,
            // but no additional jitter
            if ((thisAP->mPlay.mFramesSincePositionUpdate -= thisAP->mPlay.mFrameUpdatePeriod) >=
                    frameUpdatePeriod) {
                thisAP->mPlay.mFramesSincePositionUpdate %= frameUpdatePeriod;
            }
            headAtNewPos = true;
        }
    }
    if (0 < count) {
        object_unlock_exclusive(&thisAP->mObject);
        SLuint32 size = (SLuint32) (count * sizeof(short));
        result = IBufferQueue_Enqueue(caller, pBuffer, size);
        // not much we can do if the Enqueue fails, so we'll just drop the decoded data
        if (SL_RESULT_SUCCESS != result) {
            SL_LOGE("enqueue failed 0x%lx", result);
        }
    } else {
        // FIXME This is really hosed, you can't do this anymore!
        // FIXME Need a state PAUSE_WHEN_EMPTY
        // Should not pause yet - we just ran out of new data to enqueue,
        // but there may still be (partially) full buffers in the queue.
        thisAP->mPlay.mState = SL_PLAYSTATE_PAUSED;
        this->mEOF = SL_BOOLEAN_TRUE;
        // this would result in a non-monotonically increasing position, so don't do it
        // thisAP->mPlay.mPosition = thisAP->mPlay.mDuration;
        object_unlock_exclusive_attributes(&thisAP->mObject, ATTR_TRANSPORT);
    }
    // callbacks are called with mutex unlocked
    if (NULL != callback) {
        if (headAtNewPos) {
            (*callback)(&thisAP->mPlay.mItf, context, SL_PLAYEVENT_HEADATNEWPOS);
        }
    }
}


/** \brief Check whether the supplied libsndfile format is supported by us */

SLboolean SndFile_IsSupported(const SF_INFO *sfinfo)
{
    switch (sfinfo->format & SF_FORMAT_TYPEMASK) {
    case SF_FORMAT_WAV:
        break;
    default:
        return SL_BOOLEAN_FALSE;
    }
    switch (sfinfo->format & SF_FORMAT_SUBMASK) {
    case SF_FORMAT_PCM_U8:
    case SF_FORMAT_PCM_16:
        break;
    default:
        return SL_BOOLEAN_FALSE;
    }
    switch (sfinfo->samplerate) {
    case 11025:
    case 22050:
    case 44100:
        break;
    default:
        return SL_BOOLEAN_FALSE;
    }
    switch (sfinfo->channels) {
    case 1:
    case 2:
        break;
    default:
        return SL_BOOLEAN_FALSE;
    }
    return SL_BOOLEAN_TRUE;
}


/** \brief Check whether the partially-constructed AudioPlayer is compatible with libsndfile */

SLresult SndFile_checkAudioPlayerSourceSink(CAudioPlayer *this)
{
    const SLDataSource *pAudioSrc = &this->mDataSource.u.mSource;
    SLuint32 locatorType = *(SLuint32 *)pAudioSrc->pLocator;
    SLuint32 formatType = *(SLuint32 *)pAudioSrc->pFormat;
    switch (locatorType) {
    case SL_DATALOCATOR_BUFFERQUEUE:
        break;
    case SL_DATALOCATOR_URI:
        {
        SLDataLocator_URI *dl_uri = (SLDataLocator_URI *) pAudioSrc->pLocator;
        SLchar *uri = dl_uri->URI;
        if (NULL == uri) {
            return SL_RESULT_PARAMETER_INVALID;
        }
        if (!strncmp((const char *) uri, "file:///", 8)) {
            uri += 8;
        }
        switch (formatType) {
        case SL_DATAFORMAT_NULL:    // OK to omit the data format
        case SL_DATAFORMAT_MIME:    // we ignore a MIME type if specified
            break;
        default:
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        this->mSndFile.mPathname = uri;
        this->mBufferQueue.mNumBuffers = SndFile_NUMBUFS;
        }
        break;
    default:
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    this->mSndFile.mWhich = 0;
    this->mSndFile.mSNDFILE = NULL;
    // this->mSndFile.mMutex is initialized only when there is a valid mSNDFILE
    this->mSndFile.mEOF = SL_BOOLEAN_FALSE;

    return SL_RESULT_SUCCESS;
}


/** \brief Called with mutex unlocked for marker and position updates, and play state change */

void audioPlayerTransportUpdate(CAudioPlayer *audioPlayer)
{
    // FIXME should use two separate hooks since we have separate attributes TRANSPORT and POSITION

    if (NULL != audioPlayer->mSndFile.mSNDFILE) {

        object_lock_exclusive(&audioPlayer->mObject);
        SLboolean empty = 0 == audioPlayer->mBufferQueue.mState.count;
        // FIXME a made-up number that should depend on player state and prefetch status
        audioPlayer->mPrefetchStatus.mLevel = 1000;
        SLmillisecond pos = audioPlayer->mSeek.mPos;
        if (SL_TIME_UNKNOWN != pos) {
            audioPlayer->mSeek.mPos = SL_TIME_UNKNOWN;
            // trim seek position to the current known duration
            if (pos > audioPlayer->mPlay.mDuration) {
                pos = audioPlayer->mPlay.mDuration;
            }
            audioPlayer->mPlay.mLastSeekPosition = pos;
            audioPlayer->mPlay.mFramesSinceLastSeek = 0;
            // seek postpones the next head at new position callback
            audioPlayer->mPlay.mFramesSincePositionUpdate = 0;
        }
        object_unlock_exclusive(&audioPlayer->mObject);

        if (SL_TIME_UNKNOWN != pos) {

            // discard any enqueued buffers for the old position
            IBufferQueue_Clear(&audioPlayer->mBufferQueue.mItf);
            empty = SL_BOOLEAN_TRUE;

            pthread_mutex_lock(&audioPlayer->mSndFile.mMutex);
            // FIXME why void?
            (void) sf_seek(audioPlayer->mSndFile.mSNDFILE, (sf_count_t) (((long long) pos *
                audioPlayer->mSndFile.mSfInfo.samplerate) / 1000LL), SEEK_SET);
            audioPlayer->mSndFile.mEOF = SL_BOOLEAN_FALSE;
            audioPlayer->mSndFile.mWhich = 0;
            pthread_mutex_unlock(&audioPlayer->mSndFile.mMutex);

        }

        // FIXME only on seek or play state change (STOPPED, PAUSED) -> PLAYING
        if (empty) {
            SndFile_Callback(&audioPlayer->mBufferQueue.mItf, audioPlayer);
        }

    }

}

#endif // USE_SNDFILE
