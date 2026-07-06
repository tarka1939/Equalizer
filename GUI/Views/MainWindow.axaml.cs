using Avalonia.Controls;
using EqualizerGUI.ViewModels;

namespace EqualizerGUI.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        // Pass the StorageProvider to the ViewModel once the window is opened.
        Opened += (_, _) =>
        {
            if (DataContext is MainViewModel vm)
                vm.StorageProvider = StorageProvider;
        };
    }
}
