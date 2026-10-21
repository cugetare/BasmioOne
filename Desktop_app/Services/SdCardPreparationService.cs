using System.Diagnostics;
using System.IO;
using StoryboxSdPrep.Models;

namespace StoryboxSdPrep.Services;

public sealed record DriveOption(string RootPath, string DisplayName)
{
    public override string ToString() => DisplayName;
}

public static class SdCardPreparationService
{
    public static IReadOnlyList<DriveOption> GetWritableDrives()
    {
        return DriveInfo
            .GetDrives()
            .Where(static d => d.IsReady)
            .Where(static d => d.DriveType is DriveType.Removable or DriveType.Fixed)
            .Select(static d => new DriveOption(
                d.Name,
                $"{d.Name}  |  {d.VolumeLabel}  |  {d.DriveFormat}  |  {d.TotalFreeSpace / (1024 * 1024 * 1024)} GB liber"))
            .OrderBy(static d => d.RootPath, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public static async Task PrepareAsync(
        string driveRoot,
        IReadOnlyCollection<Story> stories,
        bool formatFat32,
        string fat32ToolPath,
        CancellationToken cancellationToken = default)
    {
        if (formatFat32)
        {
            await FormatFat32WithExternalToolAsync(driveRoot, fat32ToolPath, cancellationToken);
        }

        foreach (var story in stories)
        {
            cancellationToken.ThrowIfCancellationRequested();

            var storyFolderName = SanitizePathPart(story.Title, "Poveste");
            var targetFolder = Path.Combine(driveRoot, storyFolderName);

            if (Directory.Exists(targetFolder))
            {
                Directory.Delete(targetFolder, recursive: true);
            }

            Directory.CreateDirectory(targetFolder);

            var chapters = story.GetOrderedChapters();
            for (var index = 0; index < chapters.Count; index++)
            {
                var chapter = chapters[index];
                var chapterTitle = string.IsNullOrWhiteSpace(chapter.Title)
                    ? Path.GetFileNameWithoutExtension(chapter.OriginalFileName)
                    : chapter.Title;

                var chapterFileName = $"{index + 1:00} - {SanitizePathPart(chapterTitle, $"Capitol {index + 1}")}.mp3";
                var chapterTargetPath = Path.Combine(targetFolder, chapterFileName);

                File.Copy(chapter.SourcePath, chapterTargetPath, overwrite: true);
            }

            if (story.CoverPngData is not null)
            {
                var coverPath = Path.Combine(targetFolder, "cover.png");
                await File.WriteAllBytesAsync(coverPath, story.CoverPngData, cancellationToken);
            }
        }
    }

    private static async Task FormatFat32WithExternalToolAsync(string driveRoot, string fat32ToolPath, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(fat32ToolPath) || !File.Exists(fat32ToolPath))
        {
            throw new InvalidOperationException($"Utilitar FAT32 inexistent: {fat32ToolPath}");
        }

        var drive = driveRoot.TrimEnd('\\');
        var startInfo = new ProcessStartInfo
        {
            FileName = fat32ToolPath,
            Arguments = drive,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            CreateNoWindow = true
        };

        using var process = new Process { StartInfo = startInfo };
        process.Start();

        // Majoritatea utilitarelor cer confirmare; trimitem "y".
        await process.StandardInput.WriteLineAsync("y");
        process.StandardInput.Close();

        var outputTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var errorTask = process.StandardError.ReadToEndAsync(cancellationToken);

        await process.WaitForExitAsync(cancellationToken);
        var output = await outputTask;
        var error = await errorTask;

        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Formatarea FAT32 a esuat (cod {process.ExitCode}).\n{output}\n{error}".Trim());
        }
    }

    private static string SanitizePathPart(string value, string fallback)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var sanitized = new string(value
            .Select(ch => invalid.Contains(ch) ? '_' : ch)
            .ToArray())
            .Trim();

        if (string.IsNullOrWhiteSpace(sanitized))
        {
            return fallback;
        }

        return sanitized.TrimEnd('.', ' ');
    }
}
