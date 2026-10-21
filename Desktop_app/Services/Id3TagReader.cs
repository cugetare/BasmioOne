using System.IO;
using System.Text;

namespace StoryboxSdPrep.Services;

public sealed record Mp3Metadata(string? Title, int? TrackNumber);

public static class Id3TagReader
{
    public static Mp3Metadata ReadMetadata(string filePath)
    {
        try
        {
            using var stream = File.OpenRead(filePath);
            var fromV2 = TryReadId3v2(stream);
            if (fromV2 is not null)
            {
                return fromV2;
            }

            var fromV1 = TryReadId3v1(stream);
            if (fromV1 is not null)
            {
                return fromV1;
            }
        }
        catch
        {
            // Fallback to filename when tag read fails.
        }

        return new Mp3Metadata(null, null);
    }

    private static Mp3Metadata? TryReadId3v2(Stream stream)
    {
        if (stream.Length < 10)
        {
            return null;
        }

        var header = new byte[10];
        if (stream.Read(header, 0, header.Length) != header.Length)
        {
            return null;
        }

        if (header[0] != (byte)'I' || header[1] != (byte)'D' || header[2] != (byte)'3')
        {
            return null;
        }

        var majorVersion = header[3];
        if (majorVersion is < 2 or > 4)
        {
            return null;
        }

        var size = ReadSynchsafeInt(header.AsSpan(6, 4));
        if (size <= 0 || size > stream.Length - 10)
        {
            return null;
        }

        var tagData = new byte[size];
        if (stream.Read(tagData, 0, size) != size)
        {
            return null;
        }

        string? title = null;
        int? trackNumber = null;

        var position = 0;
        while (position < tagData.Length)
        {
            if (majorVersion == 2)
            {
                if (position + 6 > tagData.Length)
                {
                    break;
                }

                var frameId = Encoding.ASCII.GetString(tagData, position, 3);
                var frameSize = (tagData[position + 3] << 16) | (tagData[position + 4] << 8) | tagData[position + 5];
                if (string.IsNullOrWhiteSpace(frameId.Trim('\0')) || frameSize <= 0)
                {
                    break;
                }

                position += 6;
                if (position + frameSize > tagData.Length)
                {
                    break;
                }

                var payload = tagData.AsSpan(position, frameSize);
                ParseFrame(frameId, payload, ref title, ref trackNumber);
                position += frameSize;
            }
            else
            {
                if (position + 10 > tagData.Length)
                {
                    break;
                }

                var frameId = Encoding.ASCII.GetString(tagData, position, 4);
                var frameSizeBytes = tagData.AsSpan(position + 4, 4);
                var frameSize = majorVersion == 4
                    ? ReadSynchsafeInt(frameSizeBytes)
                    : ReadBigEndianInt(frameSizeBytes);

                if (string.IsNullOrWhiteSpace(frameId.Trim('\0')) || frameSize <= 0)
                {
                    break;
                }

                position += 10;
                if (position + frameSize > tagData.Length)
                {
                    break;
                }

                var payload = tagData.AsSpan(position, frameSize);
                ParseFrame(frameId, payload, ref title, ref trackNumber);
                position += frameSize;
            }

            if (title is not null && trackNumber.HasValue)
            {
                break;
            }
        }

        if (title is null && !trackNumber.HasValue)
        {
            return null;
        }

        return new Mp3Metadata(title, trackNumber);
    }

    private static Mp3Metadata? TryReadId3v1(Stream stream)
    {
        if (stream.Length < 128)
        {
            return null;
        }

        stream.Seek(-128, SeekOrigin.End);
        var data = new byte[128];
        if (stream.Read(data, 0, data.Length) != data.Length)
        {
            return null;
        }

        if (data[0] != (byte)'T' || data[1] != (byte)'A' || data[2] != (byte)'G')
        {
            return null;
        }

        var latin1 = Encoding.GetEncoding(28591);
        var titleRaw = latin1.GetString(data, 3, 30).TrimEnd('\0', ' ');
        var title = string.IsNullOrWhiteSpace(titleRaw) ? null : titleRaw;

        int? track = null;
        if (data[125] == 0 && data[126] > 0)
        {
            track = data[126];
        }

        if (title is null && !track.HasValue)
        {
            return null;
        }

        return new Mp3Metadata(title, track);
    }

    private static void ParseFrame(string frameId, ReadOnlySpan<byte> payload, ref string? title, ref int? trackNumber)
    {
        if (frameId is "TIT2" or "TT2")
        {
            var text = DecodeTextFrame(payload);
            if (!string.IsNullOrWhiteSpace(text))
            {
                title = text;
            }
        }

        if (frameId is "TRCK" or "TRK")
        {
            var text = DecodeTextFrame(payload);
            if (string.IsNullOrWhiteSpace(text))
            {
                return;
            }

            var firstPart = text.Split('/')[0];
            if (int.TryParse(firstPart.Trim(), out var parsedTrack))
            {
                trackNumber = parsedTrack;
            }
        }
    }

    private static string DecodeTextFrame(ReadOnlySpan<byte> payload)
    {
        if (payload.Length == 0)
        {
            return string.Empty;
        }

        var encodingByte = payload[0];
        var body = payload.Slice(1).ToArray();

        Encoding encoding = encodingByte switch
        {
            0 => Encoding.GetEncoding(28591),
            1 => Encoding.Unicode,
            2 => Encoding.BigEndianUnicode,
            3 => Encoding.UTF8,
            _ => Encoding.UTF8
        };

        return encoding.GetString(body).TrimEnd('\0', ' ');
    }

    private static int ReadSynchsafeInt(ReadOnlySpan<byte> bytes)
        => (bytes[0] << 21) | (bytes[1] << 14) | (bytes[2] << 7) | bytes[3];

    private static int ReadBigEndianInt(ReadOnlySpan<byte> bytes)
        => (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}
