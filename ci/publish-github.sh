#!/bin/sh
# Publishes a release tag to the public GitHub repo as one new commit on its main branch: the
# tag's files (without .gitea/), the tag's message as release notes, and the public author.
# The private history is never pushed. Then it creates a GitHub release with the bundle.
#
# Environment:
#   TAG             the release tag, e.g. v1.0.0; its annotation becomes the release notes
#   GITHUB_REPO     owner/name of the public repo
#   GITHUB_TOKEN    token with write access to its contents
#   AUTHOR_NAME     public author and committer, e.g. IgnatzMcJockel
#   AUTHOR_EMAIL    their public address, e.g. GitHub's noreply address
#   DENYLIST        extended regex (case-insensitive) that must match nothing published
#   ASSET           the zipped bundle to attach to the release
#   PUBLIC_URL      optional: where to push instead of GitHub (for tests)
#   SKIP_RELEASE    optional: 1 skips the GitHub release (for tests)
set -e

for name in TAG GITHUB_REPO AUTHOR_NAME AUTHOR_EMAIL DENYLIST ASSET; do
    eval "value=\${$name:-}"
    [ -n "$value" ] || { echo "$name isn't set" >&2; exit 1; }
done
url=${PUBLIC_URL:-https://x-access-token:$GITHUB_TOKEN@github.com/$GITHUB_REPO.git}

# The tag object, for its message; actions/checkout only fetches the commit.
git fetch -q --depth 1 origin "+refs/tags/$TAG:refs/tags/$TAG"
notes=$(git tag -l --format='%(contents)' "$TAG")
message="Twin Crowns $TAG${notes:+

$notes}"

# The published tree: the tag's tree without the CI workflows of the private server.
index=$(mktemp)
trap 'rm -f "$index" "$index.msg"' EXIT
GIT_INDEX_FILE=$index git read-tree "$TAG"
GIT_INDEX_FILE=$index git rm -q -r --cached --ignore-unmatch .gitea
tree=$(GIT_INDEX_FILE=$index git write-tree)

# Nothing on the denylist may go out. Print file names only, never the match itself.
leaks=$(git grep -I -i -l -E "$DENYLIST" "$tree" -- . 2>/dev/null || true)
if [ -n "$leaks" ]; then
    echo "Denylisted text in these files, nothing published:" >&2
    echo "$leaks" | sed "s|^$tree:|  |" >&2
    exit 1
fi
if printf '%s\n%s\n%s\n' "$message" "$AUTHOR_NAME" "$AUTHOR_EMAIL" | grep -q -i -E "$DENYLIST"; then
    echo "Denylisted text in the tag message or the author, nothing published" >&2
    exit 1
fi

if git ls-remote --exit-code --tags "$url" "refs/tags/$TAG" >/dev/null 2>&1; then
    echo "$TAG is already published" >&2
    exit 1
fi

# Continue the public history, if there is one yet.
parent=$(git ls-remote "$url" refs/heads/main | cut -f1)
if [ -n "$parent" ]; then
    git fetch -q --depth 1 "$url" main
fi

printf '%s\n' "$message" > "$index.msg"
commit=$(GIT_AUTHOR_NAME=$AUTHOR_NAME GIT_AUTHOR_EMAIL=$AUTHOR_EMAIL \
         GIT_COMMITTER_NAME=$AUTHOR_NAME GIT_COMMITTER_EMAIL=$AUTHOR_EMAIL \
         git commit-tree "$tree" ${parent:+-p "$parent"} -F "$index.msg")
git push -q "$url" "$commit:refs/heads/main" "$commit:refs/tags/$TAG"
echo "Published $TAG as $commit"

[ "${SKIP_RELEASE:-0}" = 1 ] && exit 0

python3 - "$GITHUB_REPO" "$TAG" "$ASSET" <<'EOF'
import json, os, sys, urllib.request

repo, tag, asset = sys.argv[1:]
headers = {"Authorization": "Bearer " + os.environ["GITHUB_TOKEN"],
           "Accept": "application/vnd.github+json"}

def call(url, data, content_type):
    request = urllib.request.Request(url, data=data, headers=dict(headers, **{"Content-Type": content_type}))
    with urllib.request.urlopen(request) as response:
        return json.load(response)

notes = os.popen(f"git tag -l --format='%(contents)' {tag}").read().strip()
release = call(f"https://api.github.com/repos/{repo}/releases",
               json.dumps({"tag_name": tag, "name": f"Twin Crowns {tag}", "body": notes}).encode(),
               "application/json")
name = os.path.basename(asset)
with open(asset, "rb") as f:
    call(f"https://uploads.github.com/repos/{repo}/releases/{release['id']}/assets?name={name}",
         f.read(), "application/zip")
print("Release:", release["html_url"])
EOF
