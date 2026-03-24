using System.Text.Json.Serialization;

namespace EqualizerGUI.Models;

/// <summary>Matches the shared preset JSON schema (shared/preset_schema.json).</summary>
public sealed class EqPreset
{
    [JsonPropertyName("version")]   public int              Version   { get; set; } = 1;
    [JsonPropertyName("name")]      public string           Name      { get; set; } = "";
    [JsonPropertyName("description")]public string?         Description { get; set; }
    [JsonPropertyName("preamp_db")] public double           PreampDb  { get; set; }
    [JsonPropertyName("bands")]     public List<EqBand>     Bands     { get; set; } = new();
}

public sealed class EqBand
{
    [JsonPropertyName("hz")]      public double Hz     { get; set; }
    [JsonPropertyName("gain_db")] public double GainDb { get; set; }
    [JsonPropertyName("q")]       public double? Q     { get; set; }
}
