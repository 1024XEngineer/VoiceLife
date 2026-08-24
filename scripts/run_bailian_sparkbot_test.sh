#!/usr/bin/env bash
# Run the repeatable Bailian -> SparkBot serial tests without exposing credentials.

set -euo pipefail

usage() {
    cat <<'EOF'
用法:
  run_bailian_sparkbot_test.sh preflight [额外参数]
  run_bailian_sparkbot_test.sh wake [额外参数]
  run_bailian_sparkbot_test.sh multiturn [额外参数]

环境变量:
  BAILIAN_KEY_FILE  含有配置行和 sk-... 的文件，默认读取项目外的本地 key.txt
  SPARKBOT_SERIAL   SparkBot USB 串口，默认 /dev/cu.usbmodem14401
  BAILIAN_TTS_MODEL 默认 qwen-audio-3.0-tts-flash
  BAILIAN_TTS_VOICE 默认 longanlingxi
  BAILIAN_TEST_LOG_DIR 默认 /tmp/voicelife-bailian-tests
  BAILIAN_TEST_TEXT    preflight 使用的固定短句

key.txt 可以包含公共 API 地址等其他配置，但脚本只接受第一条完整的 sk- 行。
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

MODE="$1"
shift
case "$MODE" in
    preflight|wake|multiturn) ;;
    *) usage >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KEY_FILE="${BAILIAN_KEY_FILE:-/Users/mac/Desktop/project/语音模型调用/key.txt}"
SERIAL_PORT="${SPARKBOT_SERIAL:-/dev/cu.usbmodem14401}"
TTS_MODEL="${BAILIAN_TTS_MODEL:-qwen-audio-3.0-tts-flash}"
TTS_VOICE="${BAILIAN_TTS_VOICE:-longanlingxi}"
LOG_DIR="${BAILIAN_TEST_LOG_DIR:-/tmp/voicelife-bailian-tests}"

if [[ ! -r "$KEY_FILE" ]]; then
    echo "配置文件不可读: $KEY_FILE" >&2
    exit 2
fi

# key.txt is a small local profile, not a dotenv file: the first two lines can
# be API URLs. Strip whitespace/CR first and never use the whole file as
# DASHSCOPE_API_KEY.
API_KEY="$(awk '{ line=$0; gsub(/^[[:space:]]+|[[:space:]]+$/, "", line); if (line ~ /^sk-[[:alnum:]_-]+$/) { print line; exit } }' "$KEY_FILE")"
if [[ -z "$API_KEY" ]]; then
    echo "配置文件中没有找到完整的 sk- API Key: $KEY_FILE" >&2
    exit 2
fi

mkdir -p "$LOG_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
export DASHSCOPE_API_KEY="$API_KEY"

METADATA_FILE="$LOG_DIR/$MODE-$STAMP.meta.txt"
{
    printf 'mode=%s\n' "$MODE"
    printf 'tts_model=%s\n' "$TTS_MODEL"
    printf 'tts_voice=%s\n' "$TTS_VOICE"
    printf 'serial=%s\n' "$SERIAL_PORT"
    printf 'api_key_source=first_matching_sk_line\n'
    printf 'key_file=%s\n' "$KEY_FILE"
    if git -C "$REPO_DIR" rev-parse --short HEAD >/dev/null 2>&1; then
        printf 'firmware_source_commit=%s\n' "$(git -C "$REPO_DIR" rev-parse --short HEAD)"
    fi
} > "$METADATA_FILE"

if ! python3 -c 'import dashscope' >/dev/null 2>&1; then
    echo "缺少 dashscope Python 依赖，请先安装项目测试环境" >&2
    exit 2
fi

if [[ "$MODE" == preflight ]]; then
    RESULT_FILE="$LOG_DIR/preflight-$STAMP.json"
    exec python3 "$REPO_DIR/scripts/voice_bailian_load_test.py" \
        --mode tts \
        --requests 1 \
        --concurrency 1 \
        --turns-per-conversation 1 \
        --tts-model "$TTS_MODEL" \
        --voice "$TTS_VOICE" \
        --text "${BAILIAN_TEST_TEXT:-这是 SparkBot 百炼固定连通性测试。}" \
        --result-json "$RESULT_FILE" \
        "$@"
fi

if [[ ! -e "$SERIAL_PORT" ]]; then
    echo "串口不存在: $SERIAL_PORT（用 SPARKBOT_SERIAL 覆盖）" >&2
    exit 2
fi
if ! python3 -c 'import serial' >/dev/null 2>&1; then
    echo "缺少 pyserial Python 依赖，请先安装项目测试环境" >&2
    exit 2
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "缺少 ffmpeg，无法把百炼音频转换为 16 kHz PCM" >&2
    exit 2
fi

if [[ "$MODE" == wake ]]; then
    LOG_FILE="$LOG_DIR/wake-$STAMP.log"
    exec python3 "$REPO_DIR/scripts/voice_linx_wake_injection_test.py" \
        --port "$SERIAL_PORT" \
        --tts-model "$TTS_MODEL" \
        --voice "$TTS_VOICE" \
        --serial-log "$LOG_FILE" \
        "$@"
fi

LOG_FILE="$LOG_DIR/multiturn-$STAMP.log"
RESULT_FILE="$LOG_DIR/multiturn-$STAMP.json"
exec python3 "$REPO_DIR/scripts/voice_linx_serial_multiturn_test.py" \
    --port "$SERIAL_PORT" \
    --tts-model "$TTS_MODEL" \
    --voice "$TTS_VOICE" \
    --serial-log "$LOG_FILE" \
    --result-json "$RESULT_FILE" \
    "$@"
