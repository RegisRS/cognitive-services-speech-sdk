//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// audio_data_stream.cpp: Implementation definitions for CSpxAudioDataStream C++ class
//

#include "stdafx.h"
#include "create_object_helpers.h"
#include "site_helpers.h"
#include "property_id_2_name_map.h"
#include "audio_data_stream.h"
#include <algorithm>
#include <cstring>
#include <chrono>

namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {


CSpxAudioDataStream::CSpxAudioDataStream()
{
}

CSpxAudioDataStream::~CSpxAudioDataStream()
{
    Term();
}

void CSpxAudioDataStream::Term()
{
    DisconnectSynthEvents();
}

void CSpxAudioDataStream::InitFromSynthesisResult(std::shared_ptr<ISpxSynthesisResult> result)
{
    // Set request ID
    m_requestId = result->GetRequestId();

    // Set reason
    m_beginningReason = result->GetReason();
    m_latestReason = result->GetReason();

    // Set audio format
    auto requiredFormatSize = result->GetFormat(nullptr, 0);
    m_format = SpxAllocWAVEFORMATEX(requiredFormatSize);
    result->GetFormat(m_format.get(), requiredFormatSize);
    m_hasHeader = result->HasHeader();

    // Set audio data
    if (result->GetReason() != ResultReason::SynthesizingAudio)
    {
        // Shouldn't set for synthesizing event
        // Since the synthesizing event for current chunk will be fired immediately after this stream is created
        auto audioData = result->GetAudioData();
        Write(audioData->data(), (uint32_t)(audioData->size()));
    }

    if (result->GetReason() == ResultReason::SynthesizingAudioCompleted || result->GetReason() == ResultReason::Canceled)
    {
        m_writingEnded = true;

        // Set cancellation details if it's canceled result
        if (result->GetReason() == ResultReason::Canceled)
        {
            m_cancellationReason = result->GetCancellationReason();
            m_cancellationErrorCode = result->GetCancellationErrorCode();
            auto properties = SpxQueryInterface<ISpxNamedProperties>(result);
            auto cancellationDetailedText = properties->GetStringValue(GetPropertyName(PropertyId::CancellationDetails_ReasonDetailedText), "");
            SetStringValue(GetPropertyName(PropertyId::CancellationDetails_ReasonDetailedText), cancellationDetailedText.data());
        }
    }

    // Define synthesis event handling functions for audio data stream

    m_pfnSynthesizing = [this](std::shared_ptr<ISpxSynthesisEventArgs> e) {
        std::unique_lock<std::mutex> lock(m_eventMutex);

        // Check consistency of request ID, to make sure the event is handled by the right CSpxAudioDataStream instance
        auto result = e->GetResult();
        auto requestId = result->GetRequestId();
        if (strcmp(PAL::ToString(requestId).c_str(), PAL::ToString(m_requestId).c_str()) != 0)
        {
            return;
        }

        // Update reason
        m_latestReason = result->GetReason();

        auto audioData = result->GetAudioData();
        auto data = audioData->data();
        auto audioLength = (uint32_t)(e->GetResult()->GetAudioLength());
        Write(data, audioLength);
    };

    m_pfnSynthesisStopped = [this](std::shared_ptr<ISpxSynthesisEventArgs> e) {
        std::unique_lock<std::mutex> lock(m_eventMutex);

        // Check consistency of request ID, to make sure the event is handled by the right CSpxAudioDataStream instance
        auto result = e->GetResult();
        auto requestId = result->GetRequestId();
        if (strcmp(PAL::ToString(requestId).c_str(), PAL::ToString(m_requestId).c_str()) != 0)
        {
            return;
        }

        // Update reason
        m_latestReason = result->GetReason();

        // Set cancellation details if it's cancellation event
        if (result->GetReason() == ResultReason::Canceled)
        {
            m_cancellationReason = result->GetCancellationReason();
            m_cancellationErrorCode = result->GetCancellationErrorCode();
            auto properties = SpxQueryInterface<ISpxNamedProperties>(result);
            auto cancellationDetailedText = properties->GetStringValue(GetPropertyName(PropertyId::CancellationDetails_ReasonDetailedText), "");
            SetStringValue(GetPropertyName(PropertyId::CancellationDetails_ReasonDetailedText), cancellationDetailedText.data());
        }

        // Close the stream
        Close();

        auto events = result->GetEvents();

        // Disconnect synthesizing event when SynthesisCompleted/SynthesisCanceled event is fired
        events->DisconnectSynthesizingCallback((void *)this, m_pfnSynthesizing);

        // Disconnect synthesis completed event when SynthesisCompleted/SynthesisCanceled event is fired
        events->DisconnectSynthesisCompletedCallback((void *)this, m_pfnSynthesisStopped);

        // Disconnect synthesis canceled event when SynthesisCompleted/SynthesisCanceled event is fired
        events->DisconnectSynthesisCanceledCallback((void *)this, m_pfnSynthesisStopped);
    };

    // Connect events
    m_synthEvents = result->GetEvents();

    if (result->GetReason() == ResultReason::SynthesizingAudioStarted || result->GetReason() == ResultReason::SynthesizingAudio)
    {
        // Connect synthesizing event
        m_synthEvents->ConnectSynthesizingCallback((void *)this, m_pfnSynthesizing);

        // Connect synthesis completed event
        m_synthEvents->ConnectSynthesisCompletedCallback((void *)this, m_pfnSynthesisStopped);

        // Connect synthesis canceled event
        m_synthEvents->ConnectSynthesisCanceledCallback((void *)this, m_pfnSynthesisStopped);
    }
}

StreamStatus CSpxAudioDataStream::GetStatus()
{
    auto status = StreamStatus::Unknown;
    switch(m_latestReason)
    {
    case ResultReason::SynthesizingAudioStarted:
        status = StreamStatus::NoData;
        break;

    case ResultReason::SynthesizingAudio:
        if (m_inventorySize > 0)
        {
            status = StreamStatus::PartialData;
        }
        else
        {
            status = StreamStatus::NoData;
        }

        break;

    case ResultReason::SynthesizingAudioCompleted:
        if (m_beginningReason == ResultReason::SynthesizingAudio)
        {
            status = StreamStatus::PartialData;
        }
        else
        {
            status = StreamStatus::AllData;
        }

        break;

    case ResultReason::Canceled:
        status = StreamStatus::Canceled;
        break;

    default:
        break;
    }

    return status;
}

CancellationReason CSpxAudioDataStream::GetCancellationReason()
{
    return m_cancellationReason;
}

CancellationErrorCode CSpxAudioDataStream::GetCancellationErrorCode()
{
    return m_cancellationErrorCode;
}

bool CSpxAudioDataStream::CanReadData(uint32_t requestedSize)
{
    return requestedSize <= m_inventorySize - m_position;
}

bool CSpxAudioDataStream::CanReadData(uint32_t requestedSize, uint32_t pos)
{
    return pos <= m_inventorySize && requestedSize <= m_inventorySize - pos;
}

void CSpxAudioDataStream::SaveToWaveFile(const wchar_t* fileName)
{
    // Re-use the CSpxWavFileWriter class to write the audio to file
    auto waveFileWriter = SpxCreateObjectWithSite<ISpxAudioFile>("CSpxWavFileWriter", SpxGetRootSite());
    waveFileWriter->Open(fileName);

    auto formatInit = SpxQueryInterface<ISpxAudioStreamInitFormat>(waveFileWriter);
    formatInit->SetFormat(m_format.get());
    formatInit->SetHeader(m_hasHeader);

    // Re-use Read method to collect the audio data from audio list to a buffer
    auto buffer = SpxAllocSharedAudioBuffer(m_inventorySize - m_position);
    auto dataSize = Read(buffer.get(), m_inventorySize - m_position);

    auto audioOutput = SpxQueryInterface<ISpxAudioOutput>(waveFileWriter);
    audioOutput->Write(buffer.get(), dataSize);
    audioOutput->Close();
}

uint32_t CSpxAudioDataStream::GetPosistion()
{
    return m_position;
}

void CSpxAudioDataStream::SetPosition(uint32_t pos)
{
    SPX_IFTRUE_THROW_HR(pos > m_inventorySize, SPXERR_INVALID_ARG);

    m_position = pos;
}

uint32_t CSpxAudioDataStream::Read(uint8_t* buffer, uint32_t bufferSize)
{
    SPX_DBG_TRACE_VERBOSE("CSpxAudioDataStream::Read: is called");
    SPX_IFTRUE_THROW_HR(buffer == nullptr, SPXERR_INVALID_ARG);

    // Wait until either enough data is collected, or writing is finished, otherwise it's unexpected
    if (!WaitForMoreData(m_position + bufferSize) && !m_writingEnded)
    {
        SPX_THROW_HR(SPXERR_UNEXPECTED_AUDIO_OUTPUT_FAILURE);
    }

    return FillBuffer(buffer, bufferSize, m_position);
}

uint32_t CSpxAudioDataStream::Read(uint8_t* buffer, uint32_t bufferSize, uint32_t pos)
{
    SPX_DBG_TRACE_VERBOSE("CSpxAudioDataStream::Read: is called");
    SPX_IFTRUE_THROW_HR(buffer == nullptr, SPXERR_INVALID_ARG);
    SPX_IFTRUE_THROW_HR(pos > m_inventorySize, SPXERR_INVALID_ARG);

    // Wait until either enough data is collected, or writing is finished, otherwise it's unexpected
    if (!WaitForMoreData(pos + bufferSize) && !m_writingEnded)
    {
        SPX_THROW_HR(SPXERR_UNEXPECTED_AUDIO_OUTPUT_FAILURE);
    }

    return FillBuffer(buffer, bufferSize, pos);
}

uint32_t CSpxAudioDataStream::Write(uint8_t* buffer, uint32_t size)
{
    SPX_DBG_TRACE_VERBOSE("CSpxAudioDataStream::Write buffer %x size=%d", buffer, size);
    SPX_IFTRUE_THROW_HR(m_writingEnded, SPXERR_UNEXPECTED_EOF);

    if (size == 0)
    {
        return 0;
    }

    SPX_IFTRUE_THROW_HR(buffer == nullptr, SPXERR_INVALID_ARG);

    // Allocate the buffer for the audio, and make a copy of the data
    auto newBuffer = SpxAllocSharedAudioBuffer(size);
    memcpy(newBuffer.get(), buffer, size);

    // Store the buffer into our audio list
    std::unique_lock<std::mutex> lock(m_mutex);
    m_audioList.emplace_back(newBuffer, size);
    m_inventorySize += size;
    m_cv.notify_all();

    return size;
}

void CSpxAudioDataStream::Close()
{
    SignalEndOfWriting();
}

uint32_t CSpxAudioDataStream::FillBuffer(uint8_t* buffer, uint32_t bufferSize, uint32_t pos)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // Set position
    m_position = pos;

    // Calculate the count of bytes to be read
    uint32_t totalBytesToBeRead = std::min(m_inventorySize - m_position, bufferSize);
    uint32_t remainedBytesToBeRead = totalBytesToBeRead;
    uint32_t offsetInBuffer = 0;

    // Seek to m_position
    uint32_t scannedSize = 0;
    auto iterator = m_audioList.begin();
    while (iterator != m_audioList.end() && scannedSize + iterator->second <= m_position)
    {
        scannedSize += iterator->second;
        iterator++;
    }

    uint32_t positionInItem = m_position - scannedSize;

    // Read data from current item
    if (remainedBytesToBeRead > 0)
    {
        SPX_DBG_ASSERT_WITH_MESSAGE(iterator != m_audioList.end(), "m_position is out of m_audioList, which is unexpeted.");
        uint32_t bytesToBeRead = std::min(iterator->second - positionInItem, remainedBytesToBeRead);
        memcpy(buffer, iterator->first.get() + positionInItem, bytesToBeRead);
        remainedBytesToBeRead -= bytesToBeRead;
        offsetInBuffer += bytesToBeRead;
        m_position += bytesToBeRead;
    }

    if (remainedBytesToBeRead > 0)
    {
        SPX_DBG_ASSERT_WITH_MESSAGE(iterator != m_audioList.end(), "m_position is out of m_audioList, which is unexpeted.");
        iterator++;
    }

    // Read data from more items until remainedBytesToBeRead == 0
    while (iterator != m_audioList.end() && remainedBytesToBeRead > 0)
    {
        uint32_t bytesToBeRead = std::min(iterator->second, remainedBytesToBeRead);
        memcpy(buffer + offsetInBuffer, iterator->first.get(), bytesToBeRead);
        remainedBytesToBeRead -= bytesToBeRead;
        offsetInBuffer += bytesToBeRead;
        m_position += bytesToBeRead;

        iterator++;
    }

    SPX_DBG_TRACE_VERBOSE("CSpxAudioDataStream::Read: bytesRead=%d", totalBytesToBeRead);
    return totalBytesToBeRead;
}

void CSpxAudioDataStream::DisconnectSynthEvents()
{
    std::unique_lock<std::mutex> lock(m_eventMutex);

    if (m_synthEvents.get() != nullptr)
    {
        // Disconnect synthesizing event
        m_synthEvents->DisconnectSynthesizingCallback((void *)this, nullptr);

        // Disconnect synthesis completed event
        m_synthEvents->DisconnectSynthesisCompletedCallback((void *)this, nullptr);

        // Disconnect synthesis canceled event
        m_synthEvents->DisconnectSynthesisCanceledCallback((void *)this, nullptr);
    }
}


} } } } // Microsoft::CognitiveServices::Speech::Impl
