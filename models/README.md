# Models

These files are built into the executables (see `src/resources.rc`).

- `speech_embedding.bin`: Google's speech embedding model ([speech_embedding](https://www.kaggle.com/models/google/speech-embedding), originally on TFHub), licensed under the Apache License 2.0. It's converted by `tools/export_embedding.py` from the ONNX version that [openWakeWord](https://github.com/dscripka/openWakeWord) distributes (`embedding_model.onnx`, release v0.5.1). It's run by `src/embedding.cpp`.
- `sound_background.bin`: examples of sounds that aren't commands, and normalization statistics, made by `tools/make_background.ps1`. It contains 300 common English words spoken by the Windows text-to-speech voices, and synthetic noises, whole and their starts (for continuous sounds).
