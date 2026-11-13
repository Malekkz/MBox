#!/usr/bin/env bash
# Creates the seed issues on GitHub from .github/issues-seed/*.md.
# Requires the GitHub CLI (gh), authenticated, run from the repository root.
# Each seed file starts with "title:" and "labels:" lines, then "---", then the body.
set -euo pipefail

gh label create phase-0 --color 5319e7 --description "Foundations" --force
gh label create phase-1 --color 1d76db --description "Data plane" --force
gh label create needs-human-review --color d93f0b --description "A person must review before closing" --force

for f in .github/issues-seed/*.md; do
  title=$(sed -n 's/^title: //p' "$f" | head -n 1)
  labels=$(sed -n 's/^labels: //p' "$f" | head -n 1)
  body=$(mktemp)
  awk 'found { print } /^---$/ && !found { found = 1 }' "$f" > "$body"
  echo "Creating: $title"
  gh issue create --title "$title" --label "$labels" --body-file "$body"
  rm -f "$body"
done
