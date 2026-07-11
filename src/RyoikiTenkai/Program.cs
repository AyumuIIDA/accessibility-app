using RyoikiTenkai.App;
using RyoikiTenkai.Storage;

var store = new BindingStore(Path.Combine(AppContext.BaseDirectory, "bindings.json"));
var app = new RyoikiTenkaiApp(store);
app.Run();
