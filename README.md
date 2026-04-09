<img src="https://raw.githubusercontent.com/TrunkRecorder/trunk-recorder/refs/heads/master/docs/media/trunk-recorder-header.png" width="75%" height="75%">

# Trunk Recorder Whisper Transcribe Plugin <!-- omit from toc -->

This is a plugin for Trunk Recorder that sends completed call audio to a Whisper-compatible or OpenAI-compatible transcription API and stores the transcription results in the call JSON for that call.

This plugin is a **blocking** plugin. It should be configured with:

- `"pluginType": "blocking"`

This allows the plugin to run during Trunk Recorder's blocking `call_end()` stage so it can enrich the final call JSON before the call is fully completed.

The plugin supports:

- Blocking post-call transcription
- Top-level plugin enable/disable
- Per-system enable/disable
- Optional language and prompt values
- Optional segment output
- Talkgroup allow/deny filters using simple wildcard patterns
- Selectable audio source for transcription (`wav` or `m4a`)
- Default transcription input of the original `.wav` file
- Automatic fallback to `.wav` if `audioSource` is invalid
- Automatic fallback to `.wav` if `audioSource` is `m4a` but no converted file is available
- Configurable `responseFormat`
- Model-aware default `responseFormat` selection
- Safe fallback to a supported `responseFormat` for known OpenAI models
- Backward-compatible handling for unknown custom/local models
- Opt-in raw response output via `includeRawResponse`
- Clip-based audio filtering via `speechSegmentsSources` — pass keep-windows from upstream plugins (VAD, tone detection, etc.) to filter what Whisper processes
- Optional segment timestamp correction for servers that return clip-relative timestamps

Requires Trunk Recorder 5.0 or later and a Whisper-compatible or OpenAI-compatible HTTP transcription endpoint.

- [Install](#install)
- [Configure](#configure)
- [Response Format Behavior](#response-format-behavior)
- [How It Works](#how-it-works)
- [Clip-Based Audio Filtering](#clip-based-audio-filtering)
- [Talkgroup Filters](#talkgroup-filters)
- [Plugin Output](#plugin-output)
- [Example Config](#example-config)
- [Notes](#notes)

## Install

1. **Clone Trunk Recorder** source following the normal Trunk Recorder build instructions.

2. **Install libcurl development headers** if they are not already installed.

On Debian/Ubuntu:

```bash
sudo apt install libcurl4-openssl-dev
```

3. **Clone this plugin into the `user_plugins` directory** of the Trunk Recorder source tree.

```bash
cd [your trunk-recorder source directory]
cd user_plugins
git clone [your plugin repository URL]
```

4. **Build and install Trunk Recorder and the plugin**.

```bash
cd [your trunk-recorder build directory]
sudo make install
```

Plugins in `user_plugins` are built and installed along with Trunk Recorder.

## Configure

### Plugin options

| Key | Required | Default Value | Type | Description |
| --- | :---: | --- | --- | --- |
| `name` |  | `whisper_transcribe` | string | Friendly plugin name used in logs. |
| `library` | ✓ |  | string | Shared library filename or full path to the plugin, for example `libwhisper_transcribe.so`. |
| `enabled` |  | `true` | bool | Top-level plugin enable flag. If `false`, the plugin is not loaded by the plugin manager. |
| `pluginType` |  | `blocking` | string | Should be set to `blocking` for this plugin so it runs in the blocking `call_end()` stage and can enrich the final call JSON. |
| `server` | ✓ |  | string | Whisper-compatible or OpenAI-compatible transcription endpoint URL. |
| `apiKey` |  |  | string | Optional bearer token used as `Authorization: Bearer ...` when calling the transcription API. |
| `model` |  | `whisper-1` | string | Model name sent to the transcription API. This may be a standard OpenAI model or a custom/local model string. |
| `responseFormat` |  | model-dependent | string | Response format requested from the transcription API. If omitted, the plugin chooses a default based on the configured model. |
| `audioSource` |  | `wav` | string | Selects which audio file is sent to the transcription API. Supported values are `wav` and `m4a`. Defaults to `wav`. If invalid, the plugin logs a warning and falls back to `wav`. If set to `m4a` but no converted file is available, the plugin logs a warning and falls back to `wav`. |
| `timeoutSeconds` |  | `300` | integer | Total request timeout in seconds. |
| `includeRawResponse` |  | `false` | bool | When `true`, the full parsed JSON response from the transcription API is included in the plugin output as `raw_response`. Disabled by default to keep the call JSON compact. |
| `speechSegmentsSources` |  |  | array | List of upstream plugin sources that provide speech keep-windows for clip-based filtering. See [Clip-Based Audio Filtering](#clip-based-audio-filtering). |
| `adjustSegmentTimestamps` |  | `false` | bool | When `true`, corrects returned segment timestamps from concatenation-relative time back to original audio time. Only needed for servers that reset timestamps to zero when processing clipped audio. Most servers (including whisper.cpp) return absolute timestamps and do not require this. |
| `systems` | ✓ |  | array | List of system-specific settings. At least one enabled system must be configured. |

### Per-system options

Each entry in `systems` applies to a Trunk Recorder system identified by `shortName`. The plugin validates that each configured `shortName` exists in the loaded Trunk Recorder systems during initialization.

| Key | Required | Default Value | Type | Description |
| --- | :---: | --- | --- | --- |
| `shortName` | ✓ |  | string | Must match the Trunk Recorder system `shortName`. |
| `enabled` |  | `true` | bool | Enables or disables transcription for this specific system within the plugin. |
| `language` |  |  | string | Optional language hint passed to the transcription API. |
| `prompt` |  |  | string | Optional prompt passed to the transcription API. For diarization models, prompt is ignored and the plugin logs a warning. |
| `includeSegments` |  | `true` | bool | If the API returns `segments`, include them in the plugin output. |
| `talkgroupAllow` |  |  | array | Optional allow list of talkgroup patterns. If set, only matching talkgroups are transcribed. |
| `talkgroupDeny` |  |  | array | Optional deny list of talkgroup patterns. Matching talkgroups are skipped. |

### `speechSegmentsSources` entries

Each entry in `speechSegmentsSources` points to a key inside another plugin's call JSON output.

| Key | Required | Default Value | Type | Description |
| --- | :---: | --- | --- | --- |
| `plugin` | ✓ |  | string | The `name` of the upstream plugin whose call JSON output contains the segment list. |
| `key` |  | `speech_segments` | string | The key within that plugin's output that holds the `[{start, end}, ...]` array. |

## Response Format Behavior

The plugin chooses `responseFormat` in a model-aware way.

### Default response format selection

If `responseFormat` is **not** set in the config, the plugin chooses:

| Model | Default `responseFormat` |
| --- | --- |
| `whisper-1` | `verbose_json` |
| `gpt-4o-transcribe` | `json` |
| `gpt-4o-mini-transcribe` | `json` |
| `gpt-4o-transcribe-diarize` | `diarized_json` |
| Unknown/custom model | `verbose_json` |

This keeps OpenAI models on a safer default while preserving backward compatibility for local/custom Whisper-like APIs.

### Supported formats for known OpenAI models

For known OpenAI models, the plugin checks `responseFormat` and falls back if needed.

| Model | Supported `responseFormat` values |
| --- | --- |
| `whisper-1` | `json`, `text`, `srt`, `verbose_json`, `vtt` |
| `gpt-4o-transcribe` | `json`, `text` |
| `gpt-4o-mini-transcribe` | `json`, `text` |
| `gpt-4o-transcribe-diarize` | `json`, `text`, `diarized_json` |

If an unsupported format is configured for one of those known models, the plugin logs a warning and falls back to that model's default format.

For unknown/custom models, the plugin does **not** hard-reject `responseFormat`. It keeps the configured value so that local APIs with custom behavior continue to work.

### Prompt handling

For `gpt-4o-transcribe-diarize`, prompts are not supported. If a prompt is configured for that model, the plugin logs a warning and ignores it.

## How It Works

When a call ends, the plugin:

1. Runs only if the top-level plugin `enabled` flag is `true`
2. Runs as a **blocking** plugin during Trunk Recorder's blocking `call_end()` stage
3. Looks up the matching configured system by `shortName`
4. Skips calls for systems that are not enabled
5. Skips encrypted calls
6. Applies optional talkgroup allow/deny filters
7. Chooses the audio file to submit:
   - uses the original `.wav` file by default
   - uses the converted `.m4a` file only when `audioSource` is set to `m4a` and a converted file is available
   - falls back to `.wav` if `audioSource` is invalid or no converted file is available
8. Resolves the `audio_path` metadata value:
   - when `audio_archive` is `true`, uses the final captureDir archive path (`final_filename`)
   - when `audio_archive` is `false`, uses only the bare filename with no directory, since the file will not exist after processing
9. Reads clip keep-windows from upstream plugins (if `speechSegmentsSources` is configured), intersects them, and adds the result as `clip_timestamps` in the request
10. Sends the audio to the configured transcription endpoint as multipart form data
11. Stores the returned transcript and optional structured data in the plugin's section of the call JSON

The request always includes:

- `file`
- `model`
- `response_format`

It also includes:

- `language` when configured
- `prompt` when configured and supported by the selected model
- `clip_timestamps` when upstream plugins have provided segment data

Trunk Recorder runs blocking plugins directly against `call_info.call_json[plugin->name]`, which is why this plugin can enrich the final JSON output before the call is completed. It also reads from `call_info.call_json` to access output written by other plugins that ran earlier in the same blocking stage.

### Response parsing

The plugin parses responses differently depending on `responseFormat`:

- `json`, `verbose_json`, `diarized_json`
  - parses the response as JSON
  - reads `text` into `transcript`
  - stores the full parsed response in `raw_response` only when `includeRawResponse` is `true`
  - reads `segments` when present and `includeSegments` is enabled
  - includes `speaker` in segments when present
  - applies segment timestamp correction when `adjustSegmentTimestamps` is `true` and clip filtering was used
- `text`, `srt`, `vtt`
  - stores the raw response body directly in `transcript`
  - leaves `segments` empty

## Clip-Based Audio Filtering

The plugin can accept keep-window segment lists from one or more upstream plugins and use them to restrict which parts of the audio Whisper processes. This is useful for removing silence, tones, or other unwanted audio before transcription.

### How it works

Each upstream plugin (for example, a VAD plugin or a tone detection plugin) writes a list of `{start, end}` time ranges into its section of `call_json`. These represent the audio windows that plugin considers worth keeping. This plugin reads those lists and intersects them: only audio that **all** configured sources want to keep is sent to Whisper via the `clip_timestamps` request parameter.

```
Example with a VAD plugin and a tone detection plugin:

Original audio (30s):
|============================================|
0s                                          30s

VAD keeps (speech regions):
     [###]    [##########]     [######]
     2–5s       10–20s          25–30s

Tone plugin keeps (non-tone regions):
  [##############]      [###########]
      0–14s                 18–30s

Intersection (sent to Whisper as clip_timestamps):
     [##]  [######]      [##] [#####]
     2–5s  10–14s       18–20s 25–30s
```

The resulting `clip_timestamps` value sent to the API looks like:

```
2.000,5.000,10.000,14.000,18.000,20.000,25.000,30.000
```

### Segment timestamps

Most servers that support `clip_timestamps` (including whisper.cpp) return segment timestamps as positions in the **original audio file**, so the timestamps in the returned segments will accurately reflect when speech occurred in the original recording. No correction is needed in this case.

Some servers internally extract and concatenate clips before processing, and return timestamps relative to that concatenated audio starting at zero. If your server behaves this way, set `adjustSegmentTimestamps` to `true` and the plugin will remap each segment's timestamps back to their correct position in the original file.

### Upstream plugin output format

The upstream plugin must write a key containing an array of `{start, end}` objects into its section of `call_json`:

```json
{
  "my_vad_plugin": {
    "speech_segments": [
      { "start": 2.0,  "end": 5.0  },
      { "start": 10.0, "end": 20.0 },
      { "start": 25.0, "end": 30.0 }
    ]
  }
}
```

The key name defaults to `speech_segments` but is configurable per source via the `key` field in `speechSegmentsSources`.

### Missing or empty sources

If a configured source plugin has not run, produced no output, or its key is absent, that source is silently skipped. Only sources that actually produced segment data participate in the intersection. If no source produced any segments, no `clip_timestamps` is added to the request and the full audio is sent to Whisper unchanged.

### Compatibility note

`clip_timestamps` is supported by local/self-hosted Whisper-compatible servers such as whisper.cpp. It is **not** a parameter supported by the official OpenAI cloud API (`api.openai.com`). Do not configure `speechSegmentsSources` when using the OpenAI cloud endpoint.

## Talkgroup Filters

The plugin supports simple wildcard matching for talkgroups.

Supported wildcards:

- `*` matches any number of characters
- `?` matches a single character

Examples:

| Pattern | Matches |
| --- | --- |
| `1234` | Only talkgroup `1234` |
| `12*` | Any talkgroup starting with `12` |
| `56??` | Any four-digit talkgroup starting with `56` |
| `1*3` | Any talkgroup starting with `1` and ending with `3` |

Filter behavior:

- If `talkgroupAllow` is not empty, the talkgroup must match at least one allow pattern
- If `talkgroupDeny` is not empty, any matching deny pattern causes the call to be skipped
- Deny rules are applied after allow rules

## Plugin Output

This plugin writes the following keys into its per-call JSON context:

| Key | Type | Description |
| --- | --- | --- |
| `transcript` | string | The returned transcription text, or an empty string if skipped or failed. For `text`, `srt`, or `vtt`, this is the raw response body. |
| `segments` | array | Segment list from the transcription response when available and `includeSegments` is enabled. Each segment contains `id`, `start`, `end`, `text`, and optionally `speaker`. |
| `process_time_seconds` | number | Time spent processing the transcription request. |
| `skipped` | bool | Whether transcription was skipped. |
| `skip_reason` | string | Reason transcription was skipped. |
| `success` | bool | `true` if the transcription request completed successfully. |
| `error` | string | Error identifier when transcription fails. |
| `audio_source_requested` | string | The configured `audioSource` value after normalization and fallback handling. |
| `audio_source_used` | string | The actual audio type used for the request, either `wav` or `m4a`. |
| `audio_path` | string | When `audio_archive` is `true`, the full captureDir archive path where the file will land. When `audio_archive` is `false`, just the bare filename with no directory — the file will not exist after processing. Downstream plugins can read this from `call_json` instead of constructing the path themselves. |
| `response_format` | string | The response format actually requested. |
| `model` | string | The model used for transcription. |
| `raw_response` | object | The full parsed JSON response from the API. Only present when `includeRawResponse` is `true` and `responseFormat` is a JSON-based format (`json`, `verbose_json`, or `diarized_json`). |
| `clip_timestamps` | string | The `clip_timestamps` string sent to the API, e.g. `"2.000,5.000,10.000,14.000"`. Only present when at least one upstream source contributed segments. |
| `clip_count` | integer | Number of keep-window clips sent to the API after intersection. Only present when at least one upstream source contributed segments. |

Current skip reasons:

- `system_not_enabled`
- `encrypted`
- `talkgroup_filter`

Current error value on request failure:

- `transcription_request_failed`

### Example successful plugin output

```json
{
  "Whisper Transcribe": {
    "transcript": "Engine 4 respond to 123 Main Street for a reported structure fire.",
    "segments": [
      {
        "id": 0,
        "start": 0.0,
        "end": 2.8,
        "text": "Engine 4 respond to"
      },
      {
        "id": 1,
        "start": 2.8,
        "end": 6.1,
        "text": "123 Main Street for a reported structure fire."
      }
    ],
    "process_time_seconds": 1.42,
    "skipped": false,
    "skip_reason": "",
    "success": true,
    "error": "",
    "audio_source_requested": "wav",
    "audio_source_used": "wav",
    "audio_path": "300-1775743793.118_155415000.0-call_185.wav",
    "response_format": "verbose_json",
    "model": "whisper-1"
  }
}
```

### Example with clip filtering active

```json
{
  "Whisper Transcribe": {
    "transcript": "Engine 4 respond to 123 Main Street.",
    "segments": [
      { "id": 0, "start": 2.1, "end": 5.0,  "text": "Engine 4 respond to" },
      { "id": 1, "start": 10.2, "end": 14.0, "text": "123 Main Street." }
    ],
    "process_time_seconds": 0.91,
    "skipped": false,
    "skip_reason": "",
    "success": true,
    "error": "",
    "audio_source_requested": "wav",
    "audio_source_used": "wav",
    "audio_path": "300-1775743793.118_155415000.0-call_185.wav",
    "response_format": "verbose_json",
    "model": "whisper-1",
    "clip_timestamps": "2.000,5.000,10.000,14.000,18.000,20.000,25.000,30.000",
    "clip_count": 4
  }
}
```

### Example skipped plugin output

```json
{
  "Whisper Transcribe": {
    "transcript": "",
    "segments": [],
    "process_time_seconds": 0.0,
    "skipped": true,
    "skip_reason": "encrypted",
    "success": false,
    "error": "",
    "response_format": "verbose_json",
    "model": "whisper-1"
  }
}
```

## Example Config

### Default WAV transcription with `whisper-1`

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "http://127.0.0.1:8000/v1/audio/transcriptions",
      "apiKey": "",
      "model": "whisper-1",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "Public safety radio traffic. Expect unit numbers, addresses, and dispatch phrasing.",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### OpenAI `gpt-4o-mini-transcribe`

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "https://api.openai.com/v1/audio/transcriptions",
      "apiKey": "YOUR_OPENAI_API_KEY",
      "model": "gpt-4o-mini-transcribe",
      "responseFormat": "json",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### OpenAI diarization

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "https://api.openai.com/v1/audio/transcriptions",
      "apiKey": "YOUR_OPENAI_API_KEY",
      "model": "gpt-4o-transcribe-diarize",
      "responseFormat": "diarized_json",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### Local whisper.cpp with VAD and tone filtering

Two upstream plugins (`vad_plugin` and `tone_detector`) run before this plugin in the blocking stage and each write keep-window segments to their section of `call_json`. This plugin intersects those two lists and sends only the overlapping windows to Whisper.

```json
{
  "plugins": [
    {
      "name": "vad_plugin",
      "library": "libvad_plugin.so",
      "pluginType": "blocking"
    },
    {
      "name": "tone_detector",
      "library": "libtone_detector.so",
      "pluginType": "blocking"
    },
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "enabled": true,
      "pluginType": "blocking",
      "server": "http://127.0.0.1:8000/v1/audio/transcriptions",
      "model": "whisper-1",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "speechSegmentsSources": [
        { "plugin": "vad_plugin",    "key": "speech_segments" },
        { "plugin": "tone_detector", "key": "keep_segments"   }
      ],
      "adjustSegmentTimestamps": false,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

### Local/custom API with raw response output enabled

```json
{
  "plugins": [
    {
      "name": "Whisper Transcribe",
      "library": "libwhisper_transcribe.so",
      "pluginType": "blocking",
      "enabled": true,
      "server": "https://stt.example.com/v1/audio/transcriptions",
      "apiKey": "YOUR_LOCAL_API_KEY",
      "model": "largev3-gpu|analog_audio",
      "responseFormat": "verbose_json",
      "audioSource": "wav",
      "timeoutSeconds": 300,
      "includeRawResponse": true,
      "systems": [
        {
          "shortName": "county-p25",
          "enabled": true,
          "language": "en",
          "prompt": "",
          "includeSegments": true
        }
      ]
    }
  ]
}
```

If the plugin cannot be found, use a full path:

```json
{
  "name": "Whisper Transcribe",
  "library": "/usr/local/lib/trunk-recorder/libwhisper_transcribe.so",
  "enabled": true,
  "pluginType": "blocking",
  "server": "http://127.0.0.1:8000/v1/audio/transcriptions",
  "apiKey": "",
  "model": "whisper-1",
  "audioSource": "wav",
  "systems": [
    {
      "shortName": "county-p25"
    }
  ]
}
```

## Notes

- This is a **blocking** plugin and should be configured with `"pluginType": "blocking"`. If omitted, the plugin manager defaults `pluginType` to `blocking`, but setting it explicitly is clearer.
- The plugin manager also supports a top-level `"enabled"` flag for each plugin. If that is set to `false`, the plugin will not be loaded.
- The default transcription input is the original `.wav` file.
- If `audioSource` is invalid, the plugin logs a warning and falls back to `wav`.
- If `audioSource` is `m4a` but no converted file is available, the plugin logs a warning and falls back to `wav`.
- For known OpenAI models, unsupported `responseFormat` values are automatically replaced with a safe default.
- For unknown custom/local models, the plugin keeps the configured `responseFormat` to preserve compatibility.
- For diarization models, prompts are ignored.
- `raw_response` is not included in the output by default. Set `includeRawResponse` to `true` to include it.
- `audio_path` reflects the final archive path when archiving is enabled, or just the bare filename when it is not. Downstream plugins (MQTT, upload scripts, etc.) can read `call_json["Whisper Transcribe"]["audio_path"]` — or more generally `call_json[plugin_name]["audio_path"]` — instead of constructing the path themselves.
- `clip_timestamps` is only sent when `speechSegmentsSources` is configured and at least one source produced segments. If no source has data for a call, the full audio is sent unchanged.
- `clip_timestamps` is supported by local Whisper-compatible servers (e.g. whisper.cpp). It is not supported by the official OpenAI cloud API.
- `adjustSegmentTimestamps` is only needed when the transcription server resets timestamps to zero for clipped audio. Most servers return timestamps relative to the original audio file and do not require this correction.
- If the HTTP request fails or the response cannot be parsed as expected for the configured format, the plugin marks the call as unsuccessful.
- Encrypted calls are always skipped.