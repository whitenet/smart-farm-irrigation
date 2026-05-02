# Smart Farm Irrigation

GitHub-ready KiCad project for ESP32 irrigation:
- Central controller PCB
- Dual pump node PCB
- Local KiCad footprint library
- Documentation
- Gerber export folders

Open `central/CENTRAL_PRO.kicad_pro` or `node/NODE_DUAL_PRO.kicad_pro` in KiCad.
If needed, add `libraries/SmartFarm.pretty` as footprint library nickname `SmartFarm`.

Before PCB order:
1. Refill zones with `B`
2. Run DRC
3. Verify footprints against your real modules
4. Export Gerbers from KiCad
