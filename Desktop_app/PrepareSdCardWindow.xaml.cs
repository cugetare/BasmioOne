using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Windows;
using Microsoft.Win32;
using StoryboxSdPrep.Models;
using StoryboxSdPrep.Services;

namespace StoryboxSdPrep;

public partial class PrepareSdCardWindow : Window, INotifyPropertyChanged
{
    private readonly IReadOnlyCollection<Story> _stories;

    private DriveOption? _selectedDrive;
    private bool _formatFat32;
    private string _fat32ToolPath;
    private string _statusMessage = "Ready";
    private bool _isBusy;

    public PrepareSdCardWindow(IReadOnlyCollection<Story> stories)
    {
        _stories = stories;
        _fat32ToolPath = Path.Combine(AppContext.BaseDirectory, "tools", "fat32format.exe");

        InitializeComponent();
        DataContext = this;

        foreach (var title in _stories.Select(static s => s.Title))
        {
            StoryTitles.Add(title);
        }

        RefreshDrives();
    }

    public ObservableCollection<DriveOption> Drives { get; } = new();

    public ObservableCollection<string> StoryTitles { get; } = new();

    public DriveOption? SelectedDrive
    {
        get => _selectedDrive;
        set
        {
            if (Equals(_selectedDrive, value))
            {
                return;
            }

            _selectedDrive = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(CanPrepare));
        }
    }

    public bool FormatFat32
    {
        get => _formatFat32;
        set
        {
            if (_formatFat32 == value)
            {
                return;
            }

            _formatFat32 = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(CanPrepare));
        }
    }

    public string Fat32ToolPath
    {
        get => _fat32ToolPath;
        set
        {
            if (string.Equals(_fat32ToolPath, value, StringComparison.Ordinal))
            {
                return;
            }

            _fat32ToolPath = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(CanPrepare));
        }
    }

    public string StatusMessage
    {
        get => _statusMessage;
        set
        {
            if (string.Equals(_statusMessage, value, StringComparison.Ordinal))
            {
                return;
            }

            _statusMessage = value;
            OnPropertyChanged();
        }
    }

    public bool IsBusy
    {
        get => _isBusy;
        set
        {
            if (_isBusy == value)
            {
                return;
            }

            _isBusy = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(CanPrepare));
        }
    }

    public bool CanPrepare
        => !IsBusy
           && SelectedDrive is not null
           && _stories.Count > 0
           && (!FormatFat32 || !string.IsNullOrWhiteSpace(Fat32ToolPath));

    public event PropertyChangedEventHandler? PropertyChanged;

    private void RefreshDrivesButton_OnClick(object sender, RoutedEventArgs e)
        => RefreshDrives();

    private void PickFat32ToolButton_OnClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Executabile (*.exe)|*.exe",
            Multiselect = false,
            Title = "Selecteaza utilitarul FAT32"
        };

        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        Fat32ToolPath = dialog.FileName;
    }

    private async void PrepareButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!CanPrepare || SelectedDrive is null)
        {
            return;
        }

        if (FormatFat32 && !File.Exists(Fat32ToolPath))
        {
            MessageBox.Show(this, "Utilitar FAT32 invalid. Selecteaza un .exe valid.", "Prepare SD Card", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var warningText = FormatFat32
            ? $"ATENTIE: drive-ul {SelectedDrive.RootPath} va fi formatat FAT32 si apoi se vor copia povestile. Continui?"
            : $"Se vor copia {_stories.Count} povesti pe drive-ul {SelectedDrive.RootPath}. Continui?";

        var confirm = MessageBox.Show(this, warningText, "Confirmare", MessageBoxButton.YesNo, MessageBoxImage.Warning);
        if (confirm != MessageBoxResult.Yes)
        {
            return;
        }

        IsBusy = true;
        StatusMessage = "Procesare in curs...";

        try
        {
            await SdCardPreparationService.PrepareAsync(
                SelectedDrive.RootPath,
                _stories,
                FormatFat32,
                Fat32ToolPath);

            StatusMessage = "Finalizat";
            MessageBox.Show(this, "Cardul SD a fost pregatit cu succes.", "Prepare SD Card", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            StatusMessage = "Eroare";
            MessageBox.Show(this, ex.Message, "Prepare SD Card - eroare", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private void CloseButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (IsBusy)
        {
            var closeAnyway = MessageBox.Show(this, "Procesul este in curs. Inchid fereastra?", "Confirmare", MessageBoxButton.YesNo, MessageBoxImage.Question);
            if (closeAnyway != MessageBoxResult.Yes)
            {
                return;
            }
        }

        Close();
    }

    private void RefreshDrives()
    {
        var previousPath = SelectedDrive?.RootPath;
        Drives.Clear();

        foreach (var drive in SdCardPreparationService.GetWritableDrives())
        {
            Drives.Add(drive);
        }

        if (!string.IsNullOrWhiteSpace(previousPath))
        {
            SelectedDrive = Drives.FirstOrDefault(d => string.Equals(d.RootPath, previousPath, StringComparison.OrdinalIgnoreCase));
        }

        SelectedDrive ??= Drives.FirstOrDefault();
        StatusMessage = Drives.Count == 0 ? "Nu exista drive-uri disponibile" : "Ready";
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
