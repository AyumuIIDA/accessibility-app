using RyoikiTenkai.Core;
using RyoikiTenkai.Storage;
using RyoikiTenkai.Vision;
using Xunit;

namespace RyoikiTenkai.Tests;

public sealed class GestureDefinitionStoreTests
{
    [Fact]
    public void SaveAndLoad_RoundTripsCustomGestureTemplates()
    {
        var path = Path.Combine(Path.GetTempPath(), $"gestures-{Guid.NewGuid():N}.json");
        try
        {
            var template = GestureTemplateFactory.Create(GestureTemplateFactoryTests.CreateWaveSamples());
            Assert.NotNull(template);

            var store = new GestureDefinitionStore(path);
            store.Save([
                new GestureDefinition(
                    Id: "wave",
                    DisplayName: "Wave",
                    Type: "template",
                    Templates: [template],
                    CreatedAt: DateTimeOffset.UtcNow)
            ]);

            var loaded = store.Load();

            Assert.Single(loaded);
            Assert.Equal("wave", loaded[0].Id);
            Assert.Single(loaded[0].Templates);
            Assert.Equal(template.Samples.Count, loaded[0].Templates[0].Samples.Count);
        }
        finally
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }
}
