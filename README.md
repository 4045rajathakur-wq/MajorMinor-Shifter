# Major Minor Switcher (VST3 for FL Studio)

Converts a sample between major and minor in any of the 12 keys.

## Build (Windows)
1. Install Visual Studio 2022 (Desktop C++ workload), CMake 3.22+ and Git.
2. In this folder:
   ```
   cmake -B build -G "Visual Studio 17 2022"
   cmake --build build --config Release
   ```
   JUCE is downloaded automatically. The VST3 is copied to `C:\Program Files\Common Files\VST3`.
3. FL Studio: Options > Manage plugins > Find installed plugins. Add it from the mixer slot or as an effect.

## Use
- Key: tonic of the sample (C = C major or C minor). Auto detects it.
- Convert: Major to Minor, Minor to Major, or Auto (flips whatever it detects).
- Tuning: if the sample is not at A=440, enter its offset in cents.
- Max Frequency: peaks above this are untouched (mostly overtones).
- Peak Threshold: quieter peaks are left alone.
- Hold Detected Key: freezes key detection once the label shows the right key.
- The label shows the detected key, its relative major/minor, and a match %. Low % means the sample is ambiguous: set Key manually.
- Latency is 4096 samples; FL Studio compensates automatically.

## Limits
Works per spectral peak, so it suits melodic loops, pads, plucks, and chords best.
Dense full mixes and drums may get artifacts. Auto key can confuse a major key
with its relative minor, so set Key manually if the result sounds off.

## Get a one-click installer (.exe) without installing Visual Studio
1. Create a free GitHub account and a new repository.
2. Upload everything in this folder (including the hidden `.github` folder) to it.
3. Open the Actions tab, pick "Build Windows installer", click Run workflow.
4. After about 10 minutes, download `MajorMinorSwitcher-Setup` from the run's Artifacts, unzip it, and run the .exe.
5. Restart FL Studio and rescan plugins.
