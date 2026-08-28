using System.Windows.Markup;

namespace Tfrs.VoiceClient.Localization;

/// <summary>XAML markup extension: {loc:Loc SomeKey}. Language is fixed for the process's
/// lifetime (see Loc), so this resolves once at parse time — no need for a Binding/converter that
/// could react to a language change after the window is already showing.</summary>
internal sealed class LocExtension(string key) : MarkupExtension
{
    public string Key { get; set; } = key;

    public override object ProvideValue(IServiceProvider serviceProvider) => Loc.Get(Key);
}
