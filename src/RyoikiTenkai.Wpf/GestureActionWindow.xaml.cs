using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using RyoikiTenkai.Wpf.Interaction;

namespace RyoikiTenkai.Wpf;

/// <summary>
/// Saved-gesture management and action binding. This lives outside the main
/// window because the registration panel has to stay readable next to the
/// native preview, which cannot be overlaid by WPF content.
/// </summary>
public partial class GestureActionWindow : Window
{
    private readonly GestureCatalog _catalog;
    private uint _selectedId;
    private bool _suppressSelectionSync;
    private bool _initialized;

    internal GestureActionWindow(GestureCatalog catalog)
    {
        _catalog = catalog;
        // Applying SelectedIndex during InitializeComponent raises
        // SelectionChanged while later controls are still null.
        InitializeComponent();
        _initialized = true;
        _catalog.Refreshed += CatalogRefreshed;
        RenderCatalog();
    }

    /// <summary>Raised when the operator asks to replay a gesture's takes. The
    /// main window owns playback because its native handle must be destroyed
    /// before the source runtime stops.</summary>
    internal event EventHandler<SavedGestureItem>? ReviewRequested;

    internal void SelectGesture(uint definitionId)
    {
        _selectedId = definitionId;
        ApplySelection();
    }

    protected override void OnClosed(EventArgs e)
    {
        _catalog.Refreshed -= CatalogRefreshed;
        base.OnClosed(e);
    }

    private void CatalogRefreshed(object? sender, EventArgs e) => RenderCatalog();

    private void RenderCatalog()
    {
        _suppressSelectionSync = true;
        try
        {
            SavedGesturesList.Items.Clear();
            foreach (var item in _catalog.Items) SavedGesturesList.Items.Add(item);
        }
        finally
        {
            _suppressSelectionSync = false;
        }
        EmptyCatalogText.Visibility = _catalog.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ApplySelection();
        SetStatus(_catalog.Status, "TextSecondary");
    }

    private void ApplySelection()
    {
        var match = SavedGesturesList.Items.OfType<SavedGestureItem>()
            .FirstOrDefault(item => item.Id == _selectedId)
            ?? SavedGesturesList.Items.OfType<SavedGestureItem>().FirstOrDefault();
        if (!ReferenceEquals(SavedGesturesList.SelectedItem, match))
        {
            SavedGesturesList.SelectedItem = match;
        }
        ShowSelectedGesture(match);
    }

    private void ShowSelectedGesture(SavedGestureItem? item)
    {
        DetailPanel.IsEnabled = item is not null;
        if (item is null)
        {
            SelectedGestureTitleText.Text = "No gesture selected";
            SelectedGestureFactsText.Text = "Record a gesture first, then bind an action to it.";
            SelectedGestureNameText.Clear();
            GestureActionParameterText.Clear();
            GestureActionTypeComboBox.SelectedIndex = 0;
            return;
        }

        _selectedId = item.Id;
        SelectedGestureTitleText.Text = item.Name;
        SelectedGestureFactsText.Text =
            $"{item.TakeCount} accepted takes · definition {item.Id} · changes are saved by the native repository";
        SelectedGestureNameText.Text = item.Name;
        SelectedGestureEnabledCheckBox.IsChecked = item.Enabled;

        if (_catalog.BindingFor(item.Id) is { } binding)
        {
            GestureBindingEnabledCheckBox.IsChecked = binding.Enabled;
            GestureActionParameterText.Text = binding.Action.Params.Values.FirstOrDefault() ?? string.Empty;
            foreach (var option in GestureActionTypeComboBox.Items.OfType<ComboBoxItem>())
                if (StringComparer.Ordinal.Equals(option.Tag as string, binding.Action.Type))
                    GestureActionTypeComboBox.SelectedItem = option;
        }
        else
        {
            GestureBindingEnabledCheckBox.IsChecked = true;
            GestureActionParameterText.Clear();
            GestureActionTypeComboBox.SelectedIndex = 0;
        }
        UpdateActionParameterFields();
    }

    private void UpdateActionParameterFields()
    {
        if (SelectedActionType() is not { } actionType) return;
        var required = GestureCatalog.RequiresParameter(actionType);
        // Handoff actions take no value, so the empty field is hidden rather
        // than left on screen inviting input that is discarded.
        ActionParameterPanel.Visibility = required ? Visibility.Visible : Visibility.Collapsed;
        ActionParameterHintText.Text = actionType switch
        {
            "keyboard.hotkey" => "Example: CTRL+K",
            "keyboard.typeText" => "Typed into whichever window has focus.",
            "app.launch" => "Full path to an application or document.",
            "handoff.grab" => "Captures the display this window is on, as it looks when the gesture fires, and advertises it on the LAN. No value needed.",
            "handoff.release" => "Claims the latest LAN offer on this device. No value needed.",
            _ => string.Empty
        };
    }

    private void SetStatus(string message, string brushKey)
    {
        GestureRepositoryStatusText.Text = message;
        var brush = (Brush)Application.Current.Resources[brushKey];
        GestureRepositoryStatusText.Foreground = brush;
        StatusDot.Fill = brush;
    }

    private string? SelectedActionType() =>
        GestureActionTypeComboBox.SelectedItem is ComboBoxItem option && option.Tag is string type
            ? type
            : null;

    private SavedGestureItem? SelectedGesture() => SavedGesturesList.SelectedItem as SavedGestureItem;

    private bool RequireSelection(out SavedGestureItem item, string message)
    {
        if (SelectedGesture() is { } selected)
        {
            item = selected;
            return true;
        }
        SetStatus(message, "Warning");
        item = null!;
        return false;
    }

    private void SavedGesturesList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressSelectionSync || !_initialized) return;
        ShowSelectedGesture(SelectedGesture());
    }

    private void GestureActionTypeComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_initialized) return;
        UpdateActionParameterFields();
    }

    private void ReviewGestureRecordingsButton_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireSelection(out var item, "Select a saved gesture to review.")) return;
        ReviewRequested?.Invoke(this, item);
    }

    private void SaveGestureBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireSelection(out var item, "Select a saved gesture and action type.")) return;
        if (SelectedActionType() is not { } actionType)
        {
            SetStatus("Select an action type.", "Warning");
            return;
        }
        var parameter = GestureCatalog.RequiresParameter(actionType) ? GestureActionParameterText.Text : string.Empty;
        if ((GestureCatalog.RequiresParameter(actionType) && string.IsNullOrWhiteSpace(parameter))
            || Encoding.UTF8.GetByteCount(parameter) > 255)
        {
            SetStatus("Enter an action value of at most 255 UTF-8 bytes.", "Warning");
            return;
        }
        var saved = _catalog.SaveBinding(item.Id, actionType, parameter,
            GestureBindingEnabledCheckBox.IsChecked == true);
        SetStatus(_catalog.Status, saved ? "Success" : "Danger");
        if (saved) ShowSelectedGesture(item);
    }

    private void RemoveGestureBindingButton_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireSelection(out var item, "Select a saved gesture first.")) return;
        var removed = _catalog.RemoveBinding(item.Id);
        SetStatus(_catalog.Status, removed ? "Success" : "Danger");
        if (removed) ShowSelectedGesture(item);
    }

    private async void UpdateGestureDefinitionButton_Click(object sender, RoutedEventArgs e) =>
        await SaveSelectedMetadataAsync();

    private async void SelectedGestureEnabledCheckBox_Click(object sender, RoutedEventArgs e) =>
        await SaveSelectedMetadataAsync();

    private async Task SaveSelectedMetadataAsync()
    {
        if (!RequireSelection(out var item, "Select a saved gesture to update.")) return;
        var name = SelectedGestureNameText.Text.Trim();
        if (string.IsNullOrWhiteSpace(name) || Encoding.UTF8.GetByteCount(name) > 63)
        {
            SetStatus("The name must contain 1–63 UTF-8 bytes.", "Warning");
            return;
        }
        if (!_catalog.SaveMetadata(item.Id, name, SelectedGestureEnabledCheckBox.IsChecked == true))
        {
            SetStatus(_catalog.Status, "Danger");
            return;
        }
        _selectedId = item.Id;
        await _catalog.RefreshAfterCommandAsync();
    }

    private async void DeleteGestureDefinitionButton_Click(object sender, RoutedEventArgs e)
    {
        if (!RequireSelection(out var item, "Select a saved gesture to delete.")) return;
        if (MessageBox.Show(this,
                $"Delete “{item.Name}” and all {item.TakeCount} recorded takes?",
                "Delete gesture", MessageBoxButton.YesNo, MessageBoxImage.Warning)
            != MessageBoxResult.Yes) return;
        if (!_catalog.DeleteDefinition(item.Id))
        {
            SetStatus(_catalog.Status, "Danger");
            return;
        }
        _selectedId = 0;
        await _catalog.RefreshAfterCommandAsync();
    }

    private async void ReloadGestureDefinitionsButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_catalog.Reload())
        {
            SetStatus(_catalog.Status, "Danger");
            return;
        }
        await _catalog.RefreshAfterCommandAsync();
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();
}
