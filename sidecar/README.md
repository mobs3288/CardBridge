# CardBridge sidecar

A development-only BepInEx plugin for Koikatsu. It loads character cards in the real game and writes what the game built, so
CardBridge can be compared with the game numerically. It is not part of CardBridge's output and the converter never needs it.

## Use

```powershell
# build against the game's own assemblies (nothing from the game is copied into the repo)
dotnet build -c Release -o out

# deploy (the game must be closed), queue a request, start the game; it writes the dump and exits by itself
copy out\CardBridgeSidecar.dll "<game>\BepInEx\plugins\"
# io\request.txt: first line = output folder, then one card path per line
```

Per card the dump holds `renderers.json` (every renderer with its materials, colours, floats and textures), `bones.json` (world
position, rotation and scale of every transform), `mesh_<n>.bin` (skinned vertices in world space: the plugin's own skinning, then
Unity's `BakeMesh`), the texture PNGs (render textures included; textures that are sRGB come out linear), and `shot_0..2.png`.

Compare with `tools/compare_truth.py` (geometry, renderer by renderer) and `tools/compare_materials.py` (colours).

Remove `CardBridgeSidecar.dll` from `BepInEx\plugins` when done; the game does not need it.
