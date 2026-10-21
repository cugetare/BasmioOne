using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace StoryboxSdPrep.Converters;

public sealed class NullToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object parameter, CultureInfo culture)
    {
        var visibleIfNull = parameter is string text && text.Equals("Invert", StringComparison.OrdinalIgnoreCase);
        var isNull = value is null;
        var shouldBeVisible = visibleIfNull ? isNull : !isNull;
        return shouldBeVisible ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

public sealed class BoolToVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object parameter, CultureInfo culture)
    {
        var val = value is bool b && b;
        var invert = parameter is string text && text.Equals("Invert", StringComparison.OrdinalIgnoreCase);
        var visible = invert ? !val : val;
        return visible ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}