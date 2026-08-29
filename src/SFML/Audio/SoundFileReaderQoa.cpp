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
#include "SFML/Audio/SoundChannel.hpp"
#include "SFML/System/Err.hpp"
#include "SFML/System/InputStream.hpp"

#include <SFML/Audio/SoundFileReaderQoa.hpp>

#include <SFML/System/FileInputStream.hpp>

#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>


namespace
{

namespace QoaSpecs
{
std::uint32_t         magicBytes                 = 0x716F6166; // "qoaf"
constexpr std::size_t maxSampleRate              = 0xFFFFFF;
constexpr std::size_t minSampleRate              = 1;
constexpr std::size_t minChannels                = 1;
constexpr std::size_t maxChannels                = 8;
constexpr std::size_t fileHeaderSizeByte         = 8;
constexpr std::size_t frameHeaderSizeByte        = 8;
constexpr std::size_t lmsStatePerChannelSizeByte = 16;
constexpr std::size_t sliceSizeByte              = 8;
constexpr std::size_t samplesPerSlice            = 20;
constexpr std::size_t maxFrameSlicesPerChannel   = 256;

const std::array<std::vector<sf::SoundChannel>, maxChannels + 1> channelMaps = {
    std::vector<sf::SoundChannel>{},
    {
        sf::SoundChannel::Mono,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
        sf::SoundChannel::FrontCenter,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
        sf::SoundChannel::BackLeft,
        sf::SoundChannel::BackRight,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
        sf::SoundChannel::FrontCenter,
        sf::SoundChannel::BackLeft,
        sf::SoundChannel::BackRight,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
        sf::SoundChannel::FrontCenter,
        sf::SoundChannel::LowFrequencyEffects,
        sf::SoundChannel::BackLeft,
        sf::SoundChannel::BackRight,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
        sf::SoundChannel::FrontCenter,
        sf::SoundChannel::LowFrequencyEffects,
        sf::SoundChannel::BackCenter,
        sf::SoundChannel::SideLeft,
        sf::SoundChannel::SideRight,
    },
    {
        sf::SoundChannel::FrontLeft,
        sf::SoundChannel::FrontRight,
        sf::SoundChannel::FrontCenter,
        sf::SoundChannel::LowFrequencyEffects,
        sf::SoundChannel::BackLeft,
        sf::SoundChannel::BackRight,
        sf::SoundChannel::SideLeft,
        sf::SoundChannel::SideRight,
    },
};
} // namespace QoaSpecs

std::vector<sf::SoundChannel> getChannelMap(std::size_t numChannels)
{
    assert(numChannels >= QoaSpecs::minChannels && numChannels <= QoaSpecs::maxChannels);
    return QoaSpecs::channelMaps[numChannels];
}

template <typename Num>
constexpr std::int16_t clampInt16(Num value)
{
    constexpr auto min = std::numeric_limits<std::int16_t>::min();
    constexpr auto max = std::numeric_limits<std::int16_t>::max();
    return value < min ? min : value > max ? max : static_cast<std::int16_t>(value);
}

constexpr std::array<std::array<int, 8>, 16> dequantTab = {
    std::array{1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

template <typename ReturnType, typename Iter>
ReturnType readBigEndianUnsignedInt(Iter begin, std::size_t size)
{
    static_assert(std::is_unsigned_v<ReturnType>);
    static_assert(std::is_unsigned_v<std::decay_t<decltype(*begin)>>);
    assert(size >= 1 && size <= sizeof(ReturnType));
    ReturnType result = 0;
    auto       shift  = size * 8;
    for (std::size_t i = 0; i < size; ++i)
    {
        shift -= 8;
        result += static_cast<ReturnType>(static_cast<ReturnType>(*(begin + static_cast<std::ptrdiff_t>(i))) << shift);
    }
    return result;
}

template <typename ReturnType, typename Iter>
inline ReturnType readBigEndianSignedInt(Iter begin)
{
    static_assert(std::is_signed_v<ReturnType>);
    return static_cast<ReturnType>(readBigEndianUnsignedInt<std::make_unsigned_t<ReturnType>>(begin, sizeof(ReturnType)));
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
        std::array<std::uint8_t, QoaSpecs::fileHeaderSizeByte> header{};

        if (stream.read(header.data(), header.size()) != header.size())
            return std::nullopt;

        const auto magicBytes = readBigEndianUnsignedInt<std::uint32_t>(header.begin(), 4);
        if (magicBytes != QoaSpecs::magicBytes)
            return std::nullopt;

        const auto samplesPerChannel = readBigEndianUnsignedInt<std::uint32_t>(header.begin() + 4, 4);
        return HeaderContent{samplesPerChannel};
    }
};

class QoaSlice
{
public:
    QoaSlice() : QoaSlice(0) {};

    explicit QoaSlice(std::uint64_t bits) : m_bits{bits}
    {
    }

    QoaSlice& operator=(std::uint64_t bits)
    {
        m_bits = bits;
        return *this;
    }

    [[nodiscard]] std::uint8_t sfQuant() const
    {
        return static_cast<std::uint8_t>(m_bits >> 60);
    }

    [[nodiscard]] std::uint8_t qr0x(std::size_t x) const
    {
        assert(x < 20);
        return static_cast<std::uint8_t>((m_bits >> (3 * (19 - x))) & 7);
    }

private:
    std::uint64_t m_bits;
};

struct FrameContent
{
    struct Header
    {
        std::uint32_t sampleRate;
        std::uint16_t samplesPerChannel;
        std::uint16_t frameSizeByte;
        std::uint8_t  numChannels;

        [[nodiscard]] std::uint32_t slicesPerChannel() const
        {
            constexpr auto castedSamplesPerSlice = static_cast<std::uint32_t>(QoaSpecs::samplesPerSlice);
            return (samplesPerChannel + castedSamplesPerSlice - 1) / castedSamplesPerSlice;
        }

        [[nodiscard]] std::optional<std::string_view> checkError() const
        {
            if (numChannels < QoaSpecs::minChannels || numChannels > QoaSpecs::maxChannels)
                return "Number of channel in frame is out of bounds";
            if (sampleRate < QoaSpecs::minSampleRate || sampleRate > QoaSpecs::maxSampleRate)
                return "Frame sample rate is not in supported range";
            const auto slices = slicesPerChannel();
            if (slices > QoaSpecs::maxFrameSlicesPerChannel || slices <= 0)
                return "Invalid number of samples per frame";
            const auto computedFrameSizeByte = QoaSpecs::frameHeaderSizeByte +
                                               (QoaSpecs::lmsStatePerChannelSizeByte + slices * QoaSpecs::sliceSizeByte) *
                                                   numChannels;
            const auto isFrameSizeMismatch = frameSizeByte != computedFrameSizeByte;
            if (isFrameSizeMismatch)
                return "Corrupted frame data";
            return std::nullopt;
        }

        static std::optional<FrameContent::Header> readFrom(sf::InputStream& stream)
        {
            std::array<std::uint8_t, QoaSpecs::frameHeaderSizeByte> frameHeader{};
            if (stream.read(frameHeader.data(), frameHeader.size()) != frameHeader.size())
                return std::nullopt;

            const auto numChannels       = frameHeader[0];
            const auto sampleRate        = readBigEndianUnsignedInt<std::uint32_t>(frameHeader.begin() + 1, 3);
            const auto samplesPerChannel = readBigEndianUnsignedInt<std::uint16_t>(frameHeader.begin() + 4, 2);
            const auto frameSizeByte     = readBigEndianUnsignedInt<std::uint16_t>(frameHeader.begin() + 6, 2);
            return Header{sampleRate, samplesPerChannel, frameSizeByte, numChannels};
        }
    };

    struct Body
    {
        struct LmsState
        {
            std::array<std::int16_t, 4> history;
            std::array<std::int16_t, 4> weights;
        };

        std::vector<LmsState>                                                 lmsStatePerChannel;
        std::vector<std::array<QoaSlice, QoaSpecs::maxFrameSlicesPerChannel>> slicesPerChannel;

        [[nodiscard]] static std::optional<Body> readFrom(sf::InputStream& stream, const Header& header)
        {
            thread_local std::array<std::uint8_t,
                                    std::max<std::size_t>(QoaSpecs::lmsStatePerChannelSizeByte,
                                                          QoaSpecs::sliceSizeByte * QoaSpecs::maxFrameSlicesPerChannel)>
                 buffer;
            Body body;
            body.lmsStatePerChannel.resize(header.numChannels);
            for (auto& channelLmsState : body.lmsStatePerChannel)
            {
                if (stream.read(buffer.data(), QoaSpecs::lmsStatePerChannelSizeByte) != QoaSpecs::lmsStatePerChannelSizeByte)
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
            const auto slicesPerChannel = header.slicesPerChannel();
            body.slicesPerChannel.resize(header.numChannels);
            for (std::size_t sliceIndex = 0; sliceIndex < slicesPerChannel; ++sliceIndex)
            {
                for (auto& channelSlices : body.slicesPerChannel)
                {
                    if (stream.read(buffer.data(), QoaSpecs::sliceSizeByte) != QoaSpecs::sliceSizeByte)
                        return std::nullopt;
                    channelSlices[sliceIndex] = readBigEndianUnsignedInt<std::uint64_t>(buffer.begin(), QoaSpecs::sliceSizeByte);
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

std::int16_t calculateSample(std::int32_t dequantizedResidual, const FrameContent::Body::LmsState& lms)
{
    std::int64_t predictedSample = 0;
    for (std::size_t i = 0; i < lms.history.size(); ++i)
    {
        predictedSample += static_cast<std::int32_t>(lms.history[i]) * static_cast<std::int32_t>(lms.weights[i]);
    }
    predictedSample >>= 13;
    return clampInt16(predictedSample + dequantizedResidual);
}

void updateLmsState(std::int32_t dequantizedResidual, FrameContent::Body::LmsState& lms, std::int16_t sample)
{
    // Reference decoder right shifts by 4, but that is implementation defined
    const auto delta = static_cast<std::int32_t>(std::floor(dequantizedResidual / 16.0));
    for (std::size_t i = 0; i < lms.history.size(); ++i)
        lms.weights[i] += static_cast<std::int16_t>(lms.history[i] < 0 ? -delta : delta);
    for (std::size_t i = 0; i < lms.history.size() - 1; ++i)
        lms.history[i] = lms.history[i + 1];
    lms.history[lms.history.size() - 1] = sample;
}
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
                                           : std::optional{*currentStreamPosition - QoaSpecs::frameHeaderSizeByte};
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
    info.channelMap   = getChannelMap(m_channelCount);

    return info;
}


////////////////////////////////////////////////////////////
void SoundFileReaderQoa::seek(std::uint64_t sampleOffset)
{
    assert(m_inputStream && "Must open before seek");
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
    const auto containingSlicesIndexFromStart = sampleOffset / QoaSpecs::samplesPerSlice;
    const auto containingFrameIndex = containingSlicesIndexFromStart / (QoaSpecs::maxFrameSlicesPerChannel * m_channelCount);
    const auto frameSizeByte = QoaSpecs::frameHeaderSizeByte +
                               (QoaSpecs::lmsStatePerChannelSizeByte +
                                QoaSpecs::maxFrameSlicesPerChannel * QoaSpecs::sliceSizeByte) *
                                   m_channelCount;
    // Seek to the beginning of the containing frame
    const auto streamPosition = m_streamFirstFramePosition + containingFrameIndex * frameSizeByte;
    if (m_inputStream->seek(streamPosition) != streamPosition)
        err() << "Failed to seek QOA file" << std::endl;
    // Update states
    m_currentFrameSamples.clear();
    const auto previousFramesSamples = containingFrameIndex *
                                       (QoaSpecs::maxFrameSlicesPerChannel * QoaSpecs::samplesPerSlice * m_channelCount);
    m_decodedSamplesPerChannel = containingFrameIndex * QoaSpecs::maxFrameSlicesPerChannel * QoaSpecs::samplesPerSlice;
    const auto maybeError      = decodeNextFrame();
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
    m_currentFrameNextSampleIndex = sampleOffset - previousFramesSamples;
}


////////////////////////////////////////////////////////////
std::uint64_t SoundFileReaderQoa::read(std::int16_t* samples, std::uint64_t maxCount)
{
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
std::uint64_t SoundFileReaderQoa::readCurrentDecodedFrame(std::int16_t* samples, std::uint64_t maxCount)
{
    const auto remainingSamplesInFrame = m_currentFrameSamples.size() - m_currentFrameNextSampleIndex;
    if (remainingSamplesInFrame <= 0)
        return 0;
    const auto readSamplesCount = std::min<std::uint64_t>(remainingSamplesInFrame, maxCount);
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
    for (std::size_t channel = 0; channel < m_channelCount; ++channel)
    {
        auto&       lms                    = frame.body.lmsStatePerChannel[channel];
        std::size_t channelSampleNextIndex = channel;
        const auto  channelSlices          = frame.body.slicesPerChannel[channel];
        // Decode the samples in each slice
        std::uint16_t decodedSamples = 0;
        for (std::size_t sliceIndex = 0; sliceIndex < frame.header.slicesPerChannel(); ++sliceIndex)
        {
            const auto& slice                = channelSlices[sliceIndex];
            const auto  quantizedScaleFactor = slice.sfQuant();
            for (std::size_t quantizedResidualIndex = 0; quantizedResidualIndex < QoaSpecs::samplesPerSlice;
                 ++quantizedResidualIndex)
            {
                const auto dequantizedResidual = dequantTab[quantizedScaleFactor][slice.qr0x(quantizedResidualIndex)];
                const auto sample              = calculateSample(dequantizedResidual, lms);
                updateLmsState(dequantizedResidual, lms, sample);
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
