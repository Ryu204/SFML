////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2026 Laurent Gomila (laurent@sfml-dev.org)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Audio/SoundFileQoa.hpp>
#include <SFML/Audio/SoundFileReaderQoa.hpp>

#include <SFML/System/Err.hpp>
#include <SFML/System/FileInputStream.hpp>
#include <SFML/System/InputStream.hpp>

#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

#include <cassert>
#include <cstdint>
#include <cstring>


namespace
{
namespace qoa = sf::priv::qoaFile;

template <typename ReturnType, std::uint8_t ByteCount, typename Iter>
ReturnType readBigEndianUnsignedInt(Iter begin)
{
    static_assert(std::is_unsigned_v<ReturnType>);
    static_assert(std::is_same_v<std::uint8_t, std::decay_t<decltype(*begin)>>);
    static_assert(ByteCount >= 1 && ByteCount <= sizeof(ReturnType));
    ReturnType result = 0;
    for (std::size_t i = 0; i < ByteCount; ++i)
    {
        result = static_cast<ReturnType>((result << 8) | *begin);
        ++begin;
    }
    return result;
}

template <typename ReturnType, typename Iter>
inline ReturnType readBigEndianSignedInt(Iter begin)
{
    static_assert(std::is_signed_v<ReturnType>);
    return static_cast<ReturnType>(readBigEndianUnsignedInt<std::make_unsigned_t<ReturnType>, sizeof(ReturnType)>(begin));
}

struct HeaderContent
{
    std::uint32_t samplesPerChannel{};

    [[nodiscard]] bool isStreaming() const
    {
        return samplesPerChannel == 0;
    }

    static std::optional<HeaderContent> readFrom(sf::InputStream& stream)
    {
        std::array<std::uint8_t, qoa::fileHeaderSizeByte::value> header{};

        if (stream.read(header.data(), header.size()) != header.size())
            return std::nullopt;

        const auto magicBytes = readBigEndianUnsignedInt<std::uint32_t, 4>(header.begin());
        if (magicBytes != sf::priv::qoaFile::magicBytes::value)
            return std::nullopt;

        const auto samplesPerChannel = readBigEndianUnsignedInt<std::uint32_t, 4>(header.begin() + 4);
        return HeaderContent{samplesPerChannel};
    }
};

struct FrameContent
{
    struct Header
    {
        std::uint32_t sampleRate;
        std::uint16_t samplesPerChannel;
        std::uint16_t frameSizeByte;
        std::uint8_t  numChannels;

        [[nodiscard]] std::optional<std::string_view> checkError() const
        {
            if (!qoa::isChannelCountValid(numChannels))
                return "Number of channel in frame is not valid";
            if (!sf::priv::qoaFile::isSampleRateValid(sampleRate))
                return "Frame sample rate is not in supported range";
            const auto slices = qoa::samplesToSlices(samplesPerChannel);
            if (slices > qoa::maxSlicesPerChannelPerFrame::value || slices <= 0)
                return "Invalid number of samples per frame";
            const auto computedFrameSizeByte = qoa::getFrameSizeByte(numChannels, slices);
            const auto isFrameSizeMismatch   = frameSizeByte != computedFrameSizeByte;
            if (isFrameSizeMismatch)
                return "Corrupted frame data";
            return std::nullopt;
        }

        static std::optional<FrameContent::Header> readFrom(sf::InputStream& stream)
        {
            std::array<std::uint8_t, sf::priv::qoaFile::frameHeaderSizeByte::value> frameHeader{};
            if (stream.read(frameHeader.data(), frameHeader.size()) != frameHeader.size())
                return std::nullopt;

            const auto numChannels       = frameHeader[0];
            const auto sampleRate        = readBigEndianUnsignedInt<std::uint32_t, 3>(frameHeader.begin() + 1);
            const auto samplesPerChannel = readBigEndianUnsignedInt<std::uint16_t, 2>(frameHeader.begin() + 4);
            const auto frameSizeByte     = readBigEndianUnsignedInt<std::uint16_t, 2>(frameHeader.begin() + 6);
            return Header{sampleRate, samplesPerChannel, frameSizeByte, numChannels};
        }
    };

    struct Body
    {
        qoa::LmsState                                                                   lmsState;
        std::vector<std::array<qoa::QoaSlice, qoa::maxSlicesPerChannelPerFrame::value>> slicesPerChannel;

        [[nodiscard]] static std::optional<Body> readFrom(sf::InputStream& stream, const Header& header)
        {
            thread_local std::array<std::uint8_t,
                                    std::max<std::size_t>(qoa::lmsStatePerChannelSizeByte::value,
                                                          qoa::sliceSizeByte::value * qoa::maxSlicesPerChannelPerFrame::value)>
                 buffer;
            Body body;
            body.lmsState.channels.resize(header.numChannels);
            for (auto& channelLmsState : body.lmsState.channels)
            {
                if (stream.read(buffer.data(), qoa::lmsStatePerChannelSizeByte::value) !=
                    qoa::lmsStatePerChannelSizeByte::value)
                    return std::nullopt;

                auto currentIter = buffer.begin();
                for (auto& elem : channelLmsState.history)
                {
                    elem = readBigEndianSignedInt<std::int16_t>(currentIter);
                    currentIter += 2;
                }
                for (auto& elem : channelLmsState.weights)
                {
                    elem = readBigEndianSignedInt<std::int16_t>(currentIter);
                    currentIter += 2;
                }
            }
            const auto slicesPerChannel = qoa::samplesToSlices(header.samplesPerChannel);
            body.slicesPerChannel.resize(header.numChannels);
            for (std::size_t sliceIndex = 0; sliceIndex < slicesPerChannel; ++sliceIndex)
            {
                for (auto& channelSlices : body.slicesPerChannel)
                {
                    if (stream.read(buffer.data(), qoa::sliceSizeByte::value) != qoa::sliceSizeByte::value)
                        return std::nullopt;
                    channelSlices[sliceIndex] = readBigEndianUnsignedInt<std::uint64_t, qoa::sliceSizeByte::value>(
                        buffer.begin());
                }
            }
            return body;
        }

        [[nodiscard]] std::optional<std::string_view> checkError() const
        {
            return std::nullopt;
        }
    };

    Header header;
    Body   body;

    [[nodiscard]] static std::optional<FrameContent> readFrom(sf::InputStream& stream)
    {
        auto header = Header::readFrom(stream);
        if (!header || header->checkError() != std::nullopt)
            return std::nullopt;
        auto body = Body::readFrom(stream, *header);
        if (!body)
            return std::nullopt;
        return FrameContent{*header, *body};
    }

    [[nodiscard]] std::optional<std::string_view> checkError() const
    {
        if (auto err = body.checkError(); err)
            return err;
        return std::nullopt;
    }
};

} // namespace

namespace sf::priv
{
////////////////////////////////////////////////////////////
bool SoundFileReaderQoa::check(InputStream& stream)
{
    const auto headerContent = HeaderContent::readFrom(stream);
    if (!headerContent)
        return false;
    const auto frameHeader = FrameContent::Header::readFrom(stream);
    if (!frameHeader)
        return false;
    const auto frameHeaderError = frameHeader->checkError();
    return !frameHeaderError.has_value();
}


////////////////////////////////////////////////////////////
std::optional<SoundFileReader::Info> SoundFileReaderQoa::open(InputStream& stream)
{
    const auto headerContent = HeaderContent::readFrom(stream);
    if (!headerContent)
    {
        err() << "Header of QOA file was incorrectly verified or recently changed" << std::endl;
        return std::nullopt;
    }
    const auto firstFrameHeader = FrameContent::Header::readFrom(stream);
    if (!firstFrameHeader)
    {
        err() << "Failed to read first frame header in QOA file" << std::endl;
        return std::nullopt;
    }
    if (const auto error = firstFrameHeader->checkError(); error)
    {
        err() << "Invalid QOA file frame header: " << *error << std::endl;
        return std::nullopt;
    }

    // Revert stream to before frame header
    const auto currentStreamPosition = stream.tell();
    const auto beforeFramePosition   = currentStreamPosition == std::nullopt
                                           ? std::nullopt
                                           : std::optional{*currentStreamPosition - qoaFile::frameHeaderSizeByte::value};
    if (beforeFramePosition == std::nullopt || stream.seek(*beforeFramePosition) != *beforeFramePosition)
    {
        err() << "Failed to seek stream when reading QOA file";
        return std::nullopt;
    }

    m_inputStream       = &stream;
    m_channelCount      = firstFrameHeader->numChannels;
    m_sampleRate        = firstFrameHeader->sampleRate;
    m_samplesPerChannel = headerContent->samplesPerChannel;
    m_currentFrameSamples.clear();
    m_currentFrameNextSampleIndex = 0;
    m_streamFirstFramePosition    = *beforeFramePosition;

    Info info{};
    info.channelCount = m_channelCount;
    info.sampleRate   = m_sampleRate;
    info.sampleCount  = headerContent->samplesPerChannel * info.channelCount;
    info.channelMap   = qoaFile::getChannelMap(m_channelCount);

    return info;
}


////////////////////////////////////////////////////////////
void SoundFileReaderQoa::seek(std::uint64_t rawSampleOffset)
{
    assert(m_inputStream && "Must open before seek");
    if (rawSampleOffset > std::numeric_limits<std::uint32_t>::max())
    {
        err() << "Failed to seek QOA file: exceed maximum number of samples" << std::endl;
        return;
    }
    const auto sampleOffset = static_cast<std::uint32_t>(rawSampleOffset);
    if (sampleOffset >= m_samplesPerChannel * m_channelCount)
    {
        // Exoected to jump to EOF
        const auto streamSize = m_inputStream->getSize();
        if (streamSize != std::nullopt)
        {
            const auto actualPosition = m_inputStream->seek(*streamSize);
            if (actualPosition != streamSize)
                err() << "Failed to seek to EOF of QOA file" << std::endl;
        }
        // Update states
        m_decodedSamplesPerChannel = m_samplesPerChannel;
        m_currentFrameSamples.clear();
        m_currentFrameNextSampleIndex = 0;
        return;
    }

    assert(sampleOffset % m_channelCount == 0 && "Must seek to a sample of the first channel");
    const auto containingSlicesIndexFromStart = sampleOffset / qoa::samplesPerSlice::value;
    const auto containingFrameIndex           = containingSlicesIndexFromStart /
                                      (qoa::maxSlicesPerChannelPerFrame::value * m_channelCount);
    const auto frameSizeByte = static_cast<std::uint16_t>(
        qoaFile::frameHeaderSizeByte::value +
        (qoa::lmsStatePerChannelSizeByte::value + qoa::maxSlicesPerChannelPerFrame::value * qoa::sliceSizeByte::value) *
            m_channelCount);
    // Seek to the beginning of the containing frame
    const auto streamPosition = m_streamFirstFramePosition + containingFrameIndex * frameSizeByte;
    if (m_inputStream->seek(streamPosition) != streamPosition)
        err() << "Failed to seek QOA file" << std::endl;
    // Update states
    m_currentFrameSamples.clear();
    const auto previousFramesSamples = containingFrameIndex * (qoa::maxSlicesPerChannelPerFrame::value *
                                                               qoa::samplesPerSlice::value * m_channelCount);
    m_decodedSamplesPerChannel = containingFrameIndex * qoa::maxSlicesPerChannelPerFrame::value * qoa::samplesPerSlice::value;
    const auto maybeError = decodeNextFrame();
    if (maybeError != std::nullopt)
    {
        err() << "Failed to seek QOA file: " << *maybeError << std::endl;
        return;
    }
    if (previousFramesSamples + m_currentFrameSamples.size() <= sampleOffset)
    {
        err() << "Fail to seek QOA file: Actual frame has less than expected number of samples" << std::endl;
        return;
    }
    m_currentFrameNextSampleIndex = static_cast<std::uint16_t>(sampleOffset - previousFramesSamples);
}


////////////////////////////////////////////////////////////
std::uint64_t SoundFileReaderQoa::read(std::int16_t* samples, std::uint64_t rawMaxCount)
{
    const auto maxCount = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(rawMaxCount, std::numeric_limits<std::uint32_t>::max()));
    auto unfilledCount = maxCount;
    while (unfilledCount > 0)
    {
        if (m_currentFrameNextSampleIndex >= m_currentFrameSamples.size())
        {
            if (m_samplesPerChannel > 0 && m_decodedSamplesPerChannel >= m_samplesPerChannel)
                break;
            if (const auto error = decodeNextFrame(); error)
            {
                err() << "Failed to decode QOA file frame: " << *error << std::endl;
                break;
            }
        }
        const auto filledSamples = readCurrentDecodedFrame(samples, unfilledCount);
        assert(filledSamples > 0 && filledSamples <= unfilledCount);
        unfilledCount -= filledSamples;
        samples += filledSamples;
        if (unfilledCount == 0)
            break;
    }
    return maxCount - unfilledCount;
}


////////////////////////////////////////////////////////////
std::uint16_t SoundFileReaderQoa::readCurrentDecodedFrame(std::int16_t* samples, std::uint32_t maxCount)
{
    const auto remainingSamplesInFrame = static_cast<std::uint16_t>(
        m_currentFrameSamples.size() - m_currentFrameNextSampleIndex);
    if (remainingSamplesInFrame <= 0)
        return 0;
    const auto readSamplesCount = static_cast<std::uint16_t>(std::min<std::uint32_t>(remainingSamplesInFrame, maxCount));
    std::memcpy(samples, m_currentFrameSamples.data() + m_currentFrameNextSampleIndex, readSamplesCount * sizeof(*samples));
    m_currentFrameNextSampleIndex += readSamplesCount;
    return readSamplesCount;
}


////////////////////////////////////////////////////////////
std::optional<std::string_view> SoundFileReaderQoa::decodeNextFrame()
{
    // Validation
    assert(m_inputStream != nullptr && "Reader did not open any stream");
    auto maybeFrame = FrameContent::readFrom(*m_inputStream);
    if (!maybeFrame)
        return "Failed to read frame of QOA file";
    if (const auto error = maybeFrame->checkError(); error)
        return error;
    auto& frame = maybeFrame.value();
    if (frame.header.numChannels != m_channelCount)
        return "Varying number of channels per frame in QOA file not supported";
    if (frame.header.sampleRate != m_sampleRate)
        return "Varying sample rate per frame in QOA file not supported";
    const HeaderContent reconstructedHeader{m_samplesPerChannel};
    if (!reconstructedHeader.isStreaming())
    {
        assert(m_decodedSamplesPerChannel < m_samplesPerChannel && "Cannot decode past specified number of samples");
        if (frame.header.samplesPerChannel + m_decodedSamplesPerChannel > m_samplesPerChannel)
            return "Samples in frames is more than specified number of samples in header of QOA files";
    }
    // Start decoding the frame
    m_currentFrameSamples.resize(frame.header.samplesPerChannel * m_channelCount);
    const auto slicesPerChannel = qoa::samplesToSlices(frame.header.samplesPerChannel);
    auto&      lms              = frame.body.lmsState;
    for (std::uint8_t channel = 0; channel < m_channelCount; ++channel)
    {
        std::uint16_t channelSampleNextIndex = channel;
        const auto    channelSlices          = frame.body.slicesPerChannel[channel];
        // Decode the samples in each slice
        std::uint16_t decodedSamples = 0;
        for (std::uint16_t sliceIndex = 0; sliceIndex < slicesPerChannel; ++sliceIndex)
        {
            const auto& slice                = channelSlices[sliceIndex];
            const auto  quantizedScaleFactor = slice.sfQuant();
            for (std::uint8_t quantizedResidualIndex = 0; quantizedResidualIndex < qoa::samplesPerSlice::value;
                 ++quantizedResidualIndex)
            {
                const auto dequantizedResidual = qoaFile::dequantTable(quantizedScaleFactor,
                                                                       slice.qr0x(quantizedResidualIndex));
                const auto sample              = qoa::calculateSample(dequantizedResidual, lms.predictSample(channel));
                lms.updateLmsState(channel, dequantizedResidual, sample);
                // Fill samples buffer
                m_currentFrameSamples[channelSampleNextIndex] = sample;
                channelSampleNextIndex += m_channelCount;
                ++decodedSamples;
                if (decodedSamples >= frame.header.samplesPerChannel)
                    break;
            }
            if (decodedSamples >= frame.header.samplesPerChannel)
                break;
        }
    }
    m_decodedSamplesPerChannel += frame.header.samplesPerChannel;
    m_currentFrameNextSampleIndex = 0;
    return std::nullopt;
}
} // namespace sf::priv
