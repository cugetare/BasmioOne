using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Input;
using Microsoft.Win32;
using StoryboxSdPrep.Models;
using StoryboxSdPrep.Services;

namespace StoryboxSdPrep;

public partial class MainWindow : Window, INotifyPropertyChanged
{
    private Story? _selectedStory;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = this;
    }

    public ObservableCollection<Story> Stories { get; } = new();

    public Story? SelectedStory
    {
        get => _selectedStory;
        set
        {
            if (ReferenceEquals(_selectedStory, value))
            {
                return;
            }

            _selectedStory = value;
            OnPropertyChanged();
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void AddMp3MenuItem_OnClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "MP3 files (*.mp3)|*.mp3",
            Multiselect = true,
            Title = "Selecteaza fisierele MP3"
        };

        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        foreach (var filePath in dialog.FileNames)
        {
            if (!File.Exists(filePath))
            {
                continue;
            }

            var metadata = Id3TagReader.ReadMetadata(filePath);
            var audioInfo = Mp3AudioInfoReader.ReadAudioInfo(filePath);
            var title = string.IsNullOrWhiteSpace(metadata.Title)
                ? Path.GetFileNameWithoutExtension(filePath)
                : metadata.Title;

            var chapter = new Chapter
            {
                SourcePath = filePath,
                OriginalFileName = Path.GetFileName(filePath),
                Title = title,
                TrackNumber = metadata.TrackNumber,
                Duration = audioInfo.Duration,
                BitrateKbps = audioInfo.BitrateKbps
            };

            var story = new Story
            {
                Title = title
            };

            story.Chapters.Add(chapter);
            story.SortChapters();
            Stories.Add(story);
        }

        if (SelectedStory is null && Stories.Count > 0)
        {
            SelectedStory = Stories[0];
        }
    }

    private void MergeStoriesMenuItem_OnClick(object sender, RoutedEventArgs e)
    {
        var selectedStories = StoriesListBox.SelectedItems.Cast<Story>().ToList();
        if (selectedStories.Count < 2)
        {
            MessageBox.Show(this, "Selecteaza cel putin 2 povesti pentru comasare.", "Comasare", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var mergedStory = new Story
        {
            Title = BuildUniqueMergedStoryTitle(selectedStories)
        };

        var firstCover = selectedStories.FirstOrDefault(static s => s.CoverPngData is not null)?.CoverPngData;
        if (firstCover is not null)
        {
            mergedStory.SetCover(firstCover);
        }

        foreach (var chapter in selectedStories
                     .SelectMany(static story => story.Chapters)
                     .Select(static chapter => chapter.Clone()))
        {
            mergedStory.Chapters.Add(chapter);
        }

        mergedStory.SortChapters();

        var targetIndex = selectedStories
            .Select(story => Stories.IndexOf(story))
            .Where(index => index >= 0)
            .DefaultIfEmpty(Stories.Count)
            .Min();

        foreach (var story in selectedStories)
        {
            Stories.Remove(story);
        }

        if (targetIndex > Stories.Count)
        {
            targetIndex = Stories.Count;
        }

        Stories.Insert(targetIndex, mergedStory);
        SelectedStory = mergedStory;
    }

    private void DeleteStoriesMenuItem_OnClick(object sender, RoutedEventArgs e)
    {
        var selectedStories = StoriesListBox.SelectedItems.Cast<Story>().ToList();
        if (selectedStories.Count == 0)
        {
            return;
        }

        var result = MessageBox.Show(
            this,
            $"Sterg {selectedStories.Count} povesti din lista?",
            "Stergere",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);

        if (result != MessageBoxResult.Yes)
        {
            return;
        }

        foreach (var story in selectedStories)
        {
            Stories.Remove(story);
        }

        SelectedStory = Stories.FirstOrDefault();
    }

    private void PickCoverButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (SelectedStory is null)
        {
            MessageBox.Show(this, "Selecteaza o poveste mai intai.", "Cover", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var dialog = new OpenFileDialog
        {
            Filter = "Imagine (*.png;*.jpg;*.jpeg;*.bmp;*.webp)|*.png;*.jpg;*.jpeg;*.bmp;*.webp",
            Multiselect = false,
            Title = "Selecteaza cover"
        };

        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        try
        {
            var source = ImageService.LoadImageFromFile(dialog.FileName);
            var png = ImageService.ConvertToPngSquare(source, 400);
            SelectedStory.SetCover(png);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Imagine invalida: {ex.Message}", "Cover", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void PasteCoverButton_OnClick(object sender, RoutedEventArgs e)
        => TryPasteCover();

    private void ClearCoverButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (SelectedStory is null)
        {
            return;
        }

        SelectedStory.ClearCover();
    }

    private void PrepareSdCardMenuItem_OnClick(object sender, RoutedEventArgs e)
    {
        if (Stories.Count == 0)
        {
            MessageBox.Show(this, "Adauga mai intai cel putin o poveste.", "Prepare SD Card", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var window = new PrepareSdCardWindow(Stories.ToList())
        {
            Owner = this
        };

        window.ShowDialog();
    }

    private void StoriesContextMenu_OnOpened(object sender, RoutedEventArgs e)
    {
        MergeStoriesContextMenuItem.IsEnabled = StoriesListBox.SelectedItems.Count >= 2;
    }

    private void ExitMenuItem_OnClick(object sender, RoutedEventArgs e)
        => Close();

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.V || Keyboard.Modifiers != ModifierKeys.Control)
        {
            return;
        }

        if (!Clipboard.ContainsImage())
        {
            return;
        }

        if (TryPasteCover())
        {
            e.Handled = true;
        }
    }

    private bool TryPasteCover()
    {
        if (SelectedStory is null)
        {
            MessageBox.Show(this, "Selecteaza o poveste mai intai.", "Paste cover", MessageBoxButton.OK, MessageBoxImage.Information);
            return false;
        }

        if (!ImageService.TryGetImageFromClipboard(out var image) || image is null)
        {
            MessageBox.Show(this, "Clipboard-ul nu contine imagine.", "Paste cover", MessageBoxButton.OK, MessageBoxImage.Information);
            return false;
        }

        var png = ImageService.ConvertToPngSquare(image, 400);
        SelectedStory.SetCover(png);
        return true;
    }

    private string BuildUniqueMergedStoryTitle(IReadOnlyCollection<Story> sourceStories)
    {
        var baseTitle = sourceStories.FirstOrDefault()?.Title;
        if (string.IsNullOrWhiteSpace(baseTitle))
        {
            baseTitle = "Poveste comasata";
        }

        var candidate = $"{baseTitle} (comasata)";
        var suffix = 2;

        while (Stories.Any(s => string.Equals(s.Title, candidate, StringComparison.CurrentCultureIgnoreCase)))
        {
            candidate = $"{baseTitle} (comasata {suffix})";
            suffix++;
        }

        return candidate;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
