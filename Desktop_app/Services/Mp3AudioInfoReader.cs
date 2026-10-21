using System.IO;
using System.Text;

namespace StoryboxSdPrep.Services;

public sealed record Mp3AudioInfo(TimeSpan? Duration, int? BitrateKbps);

public static class Mp3AudioInfoReader
{
    private const int ScanWindowBytes = 1024 * 1024;

    private static readonly int[] BitrateMpeg1Layer3 =
    [
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    ];

    private static readonly int[] BitrateMpeg2Layer3 =
    [
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    ];

    public static Mp3AudioInfo ReadAudioInfo(string filePath)
    {
        try
        {
            using var stream = File.OpenRead(filePath);

            var audioStartOffset = SkipId3v2(stream);
            var hasId3v1 = HasId3v1(stream);

            if (!TryFindFirstFrame(stream, audioStartOffset, out var frame))
            {
                return new Mp3AudioInfo(null, null);
            }

            var durationSeconds = TryReadVbrDurationSeconds(stream, frame)
                                  ?? EstimateDurationFromSize(stream, frame, hasId3v1);

            if (durationSeconds <= 0)
            {
                return new Mp3AudioInfo(null, frame.BitrateKbps);
            }

            var audioEndOffset = stream.Length - (hasId3v1 ? 128 : 0);
            var audioBytes = Math.Max(0, audioEndOffset - frame.StartOffset);
            var averageBitrate = (int)Math.Round(audioBytes * 8d / durationSeconds / 1000d, MidpointRounding.AwayFromZero);
            var bitrate = averageBitrate > 0 ? averageBitrate : frame.BitrateKbps;

            return new Mp3AudioInfo(TimeSpan.FromSeconds(durationSeconds), bitrate);
        }
        catch
        {
            return new Mp3AudioInfo(null, null);
        }
    }

    private static long SkipId3v2(Stream stream)
    {
        stream.Position = 0;
        if (stream.Length < 10)
        {
            return 0;
        }

        var header = new byte[10];
        if (stream.Read(header, 0, header.Length) != header.Length)
        {
            return 0;
        }

        if (header[0] != (byte)'I' || header[1] != (byte)'D' || header[2] != (byte)'3')
        {
            return 0;
        }

        var size = ReadSynchsafeInt(header.AsSpan(6, 4));
        var hasFooter = (header[5] & 0x10) != 0;
        var totalTagSize = 10L + size + (hasFooter ? 10 : 0);

        if (totalTagSize < 0 || totalTagSize >= stream.Length)
        {
            return 0;
        }

        stream.Position = totalTagSize;
        return totalTagSize;
    }

    private static bool HasId3v1(Stream stream)
    {
        if (stream.Length < 128)
        {
            return false;
        }

        var previousPosition = stream.Position;
        stream.Position = stream.Length - 128;

        var tag = new byte[3];
        var read = stream.Read(tag, 0, tag.Length);
        stream.Position = previousPosition;

        return read == 3 && tag[0] == (byte)'T' && tag[1] == (byte)'A' && tag[2] == (byte)'G';
    }

    private static bool TryFindFirstFrame(Stream stream, long startOffset, out FrameInfo frame)
    {
        frame = default;

        if (stream.Length - startOffset < 4)
        {
            return false;
        }

        var bytesToRead = (int)Math.Min(ScanWindowBytes, stream.Length - startOffset);
        if (bytesToRead < 4)
        {
            return false;
        }

        var buffer = new byte[bytesToRead];
        stream.Position = startOffset;
        var actuallyRead = stream.Read(buffer, 0, bytesToRead);

        for (var i = 0; i <= actuallyRead - 4; i++)
        {
            if (!TryParseFrameHeader(buffer, i, out var candidate))
            {
                continue;
            }

            var nextFrameOffset = i + candidate.FrameLength;
            if (nextFrameOffset + 4 <= actuallyRead && !TryParseFrameHeader(buffer, nextFrameOffset, out _))
            {
                continue;
            }

            frame = candidate with { StartOffset = startOffset + i };
            return true;
        }

        return false;
    }

    private static bool TryParseFrameHeader(byte[] buffer, int offset, out FrameInfo frame)
    {
        frame = default;

        var header = ((uint)buffer[offset] << 24)
                     | ((uint)buffer[offset + 1] << 16)
                     | ((uint)buffer[offset + 2] << 8)
                     | buffer[offset + 3];

        if ((header & 0xFFE00000) != 0xFFE00000)
        {
            return false;
        }

        var versionBits = (int)((header >> 19) & 0x3);
        var layerBits = (int)((header >> 17) & 0x3);
        var bitrateIndex = (int)((header >> 12) & 0xF);
        var sampleRateIndex = (int)((header >> 10) & 0x3);
        var paddingBit = (int)((header >> 9) & 0x1);
        var channelModeBits = (int)((header >> 6) & 0x3);

        if (layerBits != 0x1 || bitrateIndex is 0 or 0xF || sampleRateIndex == 0x3)
        {
            return false;
        }

        if (!TryMapVersion(versionBits, out var version))
        {
            return false;
        }

        var sampleRate = GetSampleRate(version, sampleRateIndex);
        if (sampleRate <= 0)
        {
            return false;
        }

        var bitrate = GetBitrate(version, bitrateIndex);
        if (bitrate <= 0)
        {
            return false;
        }

        var frameLength = version == MpegVersion.Mpeg1
            ? (144000 * bitrate / sampleRate) + paddingBit
            : (72000 * bitrate / sampleRate) + paddingBit;

        if (frameLength <= 4)
        {
            return false;
        }

        frame = new FrameInfo(
            StartOffset: 0,
            Version: version,
            IsMono: channelModeBits == 0x3,
            BitrateKbps: bitrate,
            SampleRateHz: sampleRate,
            SamplesPerFrame: version == MpegVersion.Mpeg1 ? 1152 : 576,
            FrameLength: frameLength);

        return true;
    }

    private static double? TryReadVbrDurationSeconds(Stream stream, FrameInfo frame)
    {
        var xingOffset = frame.StartOffset + 4 + GetSideInfoLength(frame.Version, frame.IsMono);
        if (TryReadXingDurationSeconds(stream, xingOffset, frame, out var xingSeconds))
        {
            return xingSeconds;
        }

        var vbriOffset = frame.StartOffset + 36;
        if (TryReadVbriDurationSeconds(stream, vbriOffset, frame, out var vbriSeconds))
        {
            return vbriSeconds;
        }

        return null;
    }

    private static bool TryReadXingDurationSeconds(Stream stream, long xingOffset, FrameInfo frame, out double seconds)
    {
        seconds = 0;

        if (!TryReadAscii(stream, xingOffset, 4, out var marker)
            || (marker != "Xing" && marker != "Info"))
        {
            return false;
        }

        if (!TryReadBytes(stream, xingOffset + 4, 4, out var flagsBytes))
        {
            return false;
        }

        var flags = ReadUInt32BigEndian(flagsBytes);
        var hasFramesField = (flags & 0x1) != 0;
        if (!hasFramesField)
        {
            return false;
        }

        if (!TryReadBytes(stream, xingOffset + 8, 4, out var framesBytes))
        {
            return false;
        }

        var frames = ReadUInt32BigEndian(framesBytes);
        if (frames == 0 || frame.SampleRateHz == 0)
        {
            return false;
        }

        seconds = frames * frame.SamplesPerFrame / (double)frame.SampleRateHz;
        return seconds > 0;
    }

    private static bool TryReadVbriDurationSeconds(Stream stream, long vbriOffset, FrameInfo frame, out double seconds)
    {
        seconds = 0;

        if (!TryReadAscii(stream, vbriOffset, 4, out var marker) || marker != "VBRI")
        {
            return false;
        }

        if (!TryReadBytes(stream, vbriOffset + 14, 4, out var framesBytes))
        {
            return false;
        }

        var frames = ReadUInt32BigEndian(framesBytes);
        if (frames == 0 || frame.SampleRateHz == 0)
        {
            return false;
        }

        seconds = frames * frame.SamplesPerFrame / (double)frame.SampleRateHz;
        return seconds > 0;
    }

    private static double EstimateDurationFromSize(Stream stream, FrameInfo frame, bool hasId3v1)
    {
        var audioEndOffset = stream.Length - (hasId3v1 ? 128 : 0);
        var audioBytes = Math.Max(0, audioEndOffset - frame.StartOffset);
        return frame.BitrateKbps <= 0
            ? 0
            : audioBytes * 8d / (frame.BitrateKbps * 1000d);
    }

    private static bool TryReadAscii(Stream stream, long offset, int count, out string text)
    {
        text = string.Empty;
        if (!TryReadBytes(stream, offset, count, out var bytes))
        {
            return false;
        }

        text = Encoding.ASCII.GetString(bytes);
        return true;
    }

    private static bool TryReadBytes(Stream stream, long offset, int count, out byte[] bytes)
    {
        bytes = [];

        if (offset < 0 || count <= 0 || offset + count > stream.Length)
        {
            return false;
        }

        var previousPosition = stream.Position;
        stream.Position = offset;

        bytes = new byte[count];
        var read = stream.Read(bytes, 0, count);
        stream.Position = previousPosition;

        return read == count;
    }

    private static bool TryMapVersion(int versionBits, out MpegVersion version)
    {
        version = versionBits switch
        {
            0x3 => MpegVersion.Mpeg1,
            0x2 => MpegVersion.Mpeg2,
            0x0 => MpegVersion.Mpeg25,
            _ => default
        };

        return versionBits is 0x3 or 0x2 or 0x0;
    }

    private static int GetBitrate(MpegVersion version, int bitrateIndex)
        => version == MpegVersion.Mpeg1
            ? BitrateMpeg1Layer3[bitrateIndex]
            : BitrateMpeg2Layer3[bitrateIndex];

    private static int GetSampleRate(MpegVersion version, int sampleRateIndex)
    {
        return version switch
        {
            MpegVersion.Mpeg1 => sampleRateIndex switch
            {
                0 => 44100,
                1 => 48000,
                2 => 32000,
                _ => 0
            },
            MpegVersion.Mpeg2 => sampleRateIndex switch
            {
                0 => 22050,
                1 => 24000,
                2 => 16000,
                _ => 0
            },
            MpegVersion.Mpeg25 => sampleRateIndex switch
            {
                0 => 11025,
                1 => 12000,
                2 => 8000,
                _ => 0
            },
            _ => 0
        };
    }

    private static int GetSideInfoLength(MpegVersion version, bool isMono)
    {
        if (version == MpegVersion.Mpeg1)
        {
            return isMono ? 17 : 32;
        }

        return isMono ? 9 : 17;
    }

    private static uint ReadUInt32BigEndian(byte[] bytes)
        => ((uint)bytes[0] << 24)
           | ((uint)bytes[1] << 16)
           | ((uint)bytes[2] << 8)
           | bytes[3];

    private static int ReadSynchsafeInt(ReadOnlySpan<byte> bytes)
        => (bytes[0] << 21) | (bytes[1] << 14) | (bytes[2] << 7) | bytes[3];

    private enum MpegVersion
    {
        Mpeg25,
        Mpeg2,
        Mpeg1
    }

    private readonly record struct FrameInfo(
        long StartOffset,
        MpegVersion Version,
        bool IsMono,
        int BitrateKbps,
        int SampleRateHz,
        int SamplesPerFrame,
        int FrameLength);
}
