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
#include <optional>
#include <string_view>

#include <cassert>
#include <cstdint>


namespace
{

namespace QoaSpecs
{
std::uint32_t magicBytes = 0x716F6166; // "qoaf"

constexpr std::size_t maxSampleRate = 0xFFFFFF;
constexpr std::size_t minSampleRate = 1;

constexpr std::size_t minChannels = 1;
constexpr std::size_t maxChannels = 8;

constexpr std::size_t fileHeaderSizeByte            = 8;
constexpr std::size_t frameHeaderSizeByte           = 8;
constexpr std::size_t lmsStatePerChannelSizeByte    = 16;
constexpr std::size_t sliceSizeByte                 = 8;
constexpr std::size_t frameSlicesPerChannel         = 256;
constexpr std::size_t frameSlicesPerChannelSizeByte = frameSlicesPerChannel * sliceSizeByte;
// 1 channel audio, 16 bytes LMS state and 256 slices with 8 bytes each
constexpr std::size_t frameBodyPerChannelSizeByte = lmsStatePerChannelSizeByte + frameSlicesPerChannelSizeByte;

const std::array<std::vector<sf::SoundChannel>, maxChannels + 1> channelMaps = {
    std::vector<sf::SoundChannel>{},
    {sf::SoundChannel::Mono},
    {sf::SoundChannel::FrontLeft, sf::SoundChannel::FrontRight},
    {sf::SoundChannel::FrontLeft, sf::SoundChannel::FrontRight, sf::SoundChannel::FrontCenter},
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

std::vector<sf::SoundChannel> getChannelMap(std::size_t numChannels)
{
    assert(numChannels >= minChannels && numChannels <= maxChannels);
    return channelMaps[numChannels];
}
} // namespace QoaSpecs

template <typename ReturnType, typename Iter>
ReturnType readBigEndianUnsignedInt(Iter begin, std::size_t size)
{
    assert(size >= 1);
    ReturnType result = 0;
    auto       shift  = size * 8;
    for (std::size_t i = 0; i < size; ++i)
    {
        shift -= 8;
        result += static_cast<ReturnType>(*(begin + static_cast<std::ptrdiff_t>(i))) << shift;
    }
    return result;
}

struct HeaderContent
{
    std::uint32_t samplesPerChannel{};

    [[nodiscard]] bool isStreaming() const
    {
        return samplesPerChannel == 0;
    }

    [[nodiscard]] bool isValid() const
    {
        return samplesPerChannel >= 0;
    }

    static std::optional<HeaderContent> readFrom(sf::InputStream& stream)
    {
        std::array<std::uint8_t, QoaSpecs::fileHeaderSizeByte> header{};

        if (stream.read(header.data(), header.size()) != header.size())
            return std::nullopt;

        const auto magicBytes = readBigEndianUnsignedInt<std::uint32_t>(header.begin(), 4);
        if (magicBytes != QoaSpecs::magicBytes)
            return std::nullopt;

        const auto samplesPerChannel = readBigEndianUnsignedInt<std::uint32_t>(header.begin(), 4);
        return HeaderContent{samplesPerChannel};
    }
};

class QoaSlice
{
public:
    QoaSlice(std::uint64_t bits) : m_bits{bits}
    {
    }

    [[nodiscard]] std::uint8_t sfQuant() const
    {
        return m_bits >> 60;
    }

    [[nodiscard]] std::uint8_t qr0x(unsigned int x) const
    {
        assert(x >= 0 && x < 20);
        return (m_bits >> (3 * (19 - x))) & 3;
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

        [[nodiscard]] std::optional<std::string_view> checkError(const HeaderContent& headerContent) const
        {
            if (numChannels < QoaSpecs::minChannels || numChannels > QoaSpecs::maxChannels)
                return "Number of channel in frame is out of bounds";
            if (sampleRate < QoaSpecs::minSampleRate || sampleRate > QoaSpecs::maxSampleRate)
                return "Frame sample rate is not in supported range";
            const auto isFrameSamplesInvalid = headerContent.isStreaming() &&
                                               samplesPerChannel > headerContent.samplesPerChannel;
            const auto computedFrameSizeByte = QoaSpecs::frameHeaderSizeByte +
                                               QoaSpecs::frameBodyPerChannelSizeByte * numChannels;

            const auto isFrameSizeMismatch = frameSizeByte != computedFrameSizeByte;
            if (isFrameSamplesInvalid || isFrameSizeMismatch)
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
            std::array<std::int16_t, 4> lmsHistory;
            std::array<std::int16_t, 4> lmsWeights;
        };

        std::vector<LmsState>                                              lmsStatePerChannel;
        std::vector<std::array<QoaSlice, QoaSpecs::frameSlicesPerChannel>> slicesPerChannel;

        [[nodiscard]] static std::optional<Body> readFrom(sf::InputStream& stream)
        {
        }

        [[nodiscard]] std::optional<std::string_view> checkError() const
        {
        }
    };

    Header header;
    Body   body;

    [[nodiscard]] static std::optional<Body> readFrom(sf::InputStream& stream)
    {
    }

    [[nodiscard]] std::optional<std::string_view> checkError() const
    {
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
    if (!headerContent->isValid())
        return false;
    const auto frameHeader = FrameContent::Header::readFrom(stream);
    if (!frameHeader)
        return false;
    const auto frameHeaderError = frameHeader->checkError(*headerContent);
    return !frameHeaderError.has_value();
}


////////////////////////////////////////////////////////////
std::optional<SoundFileReader::Info> SoundFileReaderQoa::open(InputStream& stream)
{
    const auto headerContent = HeaderContent::readFrom(stream);
    if (!headerContent || !headerContent->isValid())
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
    if (const auto error = firstFrameHeader->checkError(*headerContent); error)
    {
        err() << "Invalid QOA file frame header: " << *error << std::endl;
        return std::nullopt;
    }

    m_channelCount = firstFrameHeader->numChannels;
    m_sampleRate   = firstFrameHeader->sampleRate;
    m_currentFrameSamples.clear();
    m_currentFrameNextSampleIndex = 0;

    Info info{};
    info.channelCount = m_channelCount;
    info.sampleRate   = m_sampleRate;
    info.sampleCount  = headerContent->samplesPerChannel * info.channelCount;
    info.channelMap   = QoaSpecs::getChannelMap(m_channelCount);

    return info;
}


////////////////////////////////////////////////////////////
void SoundFileReaderQoa::seek(std::uint64_t sampleOffset)
{
}


////////////////////////////////////////////////////////////
std::uint64_t SoundFileReaderQoa::read(std::int16_t* samples, std::uint64_t maxCount)
{
    return 0;
}

} // namespace sf::priv
