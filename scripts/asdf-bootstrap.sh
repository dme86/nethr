#!/usr/bin/env bash
set -euo pipefail

MODE="${1:---check}"
TOOLS_FILE=".tool-versions"
REQUIRED_TOOLS=(nodejs java gcc)

if [[ "$MODE" != "--check" && "$MODE" != "--install" ]]; then
  echo "Usage: $0 [--check|--install]"
  exit 1
fi

if ! command -v asdf >/dev/null 2>&1; then
  echo "asdf is not installed or not in PATH."
  exit 1
fi

if [[ ! -f "$TOOLS_FILE" ]]; then
  echo "Missing $TOOLS_FILE in repository root."
  exit 1
fi

plugin_installed() {
  local tool="$1"
  asdf plugin list 2>/dev/null | grep -Fxq "$tool"
}

tool_version() {
  local tool="$1"
  awk -v t="$tool" '$1 == t { print $2; exit }' "$TOOLS_FILE"
}

version_installed() {
  local tool="$1"
  local version="$2"
  asdf list "$tool" 2>/dev/null | sed -E 's/^[*[:space:]]+//' | grep -Fxq "$version"
}

for tool in "${REQUIRED_TOOLS[@]}"; do
  version="$(tool_version "$tool")"
  if [[ -z "$version" ]]; then
    echo "Missing tool entry in $TOOLS_FILE: $tool"
    exit 1
  fi

  if ! plugin_installed "$tool"; then
    if [[ "$MODE" == "--install" ]]; then
      echo "Installing asdf plugin: $tool"
      asdf plugin add "$tool"
    else
      echo "Missing asdf plugin: $tool"
      exit 1
    fi
  fi

  if ! version_installed "$tool" "$version"; then
    if [[ "$MODE" == "--install" ]]; then
      echo "Installing $tool $version"
      asdf install "$tool" "$version"
    else
      echo "Missing installed version for $tool: $version"
      exit 1
    fi
  fi
done

echo "asdf setup is OK."
