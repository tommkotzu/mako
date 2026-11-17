#!/bin/bash
# Publishes a Mako .deb to the apt repository at
# https://tommkotzu.github.io/mako-apt/ (the GitHub repository tommkotzu/mako-apt,
# served by GitHub Pages). Installed copies of Mako get it through Software
# Updater / apt upgrade.
# usage: tools/publish-apt.sh path/to/mako_VERSION_amd64.deb
# The signing key lives in ~/.config/mako-apt/gnupg (backup next to it); its
# public half is data/apt/mako-archive-keyring.gpg, shipped in the .deb.
# The repository keeps only the newest two versions, in a single commit, so
# it never grows.
set -euo pipefail
DEB=$(readlink -f "${1:?usage: $0 mako_VERSION_amd64.deb}")
REPO=tommkotzu/mako-apt
export GNUPGHOME=${MAKO_APT_GNUPG:-$HOME/.config/mako-apt/gnupg}
SRC=$(git rev-parse --show-toplevel)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# The packages published so far, to keep the previous version.
git clone -q --depth 1 "https://github.com/$REPO.git" site 2>/dev/null || mkdir site
mkdir -p site/pool/main/m/mako
cp "$DEB" site/pool/main/m/mako/
ls -1v site/pool/main/m/mako/*.deb | head -n -2 | xargs -r rm -f

cd site
rm -rf .git dists
D=dists/stable/main/binary-amd64
mkdir -p $D
dpkg-scanpackages --multiversion pool /dev/null 2>/dev/null > $D/Packages
gzip -9kn $D/Packages
{
    echo "Origin: Mako"
    echo "Label: Mako"
    echo "Suite: stable"
    echo "Codename: stable"
    echo "Architectures: amd64"
    echo "Components: main"
    echo "Description: Mako file manager"
    echo "Date: $(LC_ALL=C date -Ru)"
    echo "SHA256:"
    for f in main/binary-amd64/Packages main/binary-amd64/Packages.gz; do
        printf ' %s %s %s\n' "$(sha256sum "dists/stable/$f" | cut -d' ' -f1)" \
            "$(stat -c%s "dists/stable/$f")" "$f"
    done
} > dists/stable/Release
gpg --batch --yes --clearsign -o dists/stable/InRelease dists/stable/Release
gpg --batch --yes --armor --detach-sign -o dists/stable/Release.gpg dists/stable/Release
cp "$SRC/data/apt/mako-archive-keyring.gpg" .
touch .nojekyll
VERSION=$(dpkg-deb -f "$DEB" Version)
FPR=$(gpg --batch --with-colons --fingerprint | awk -F: '/^fpr/{print $10; exit}')
cat > index.html <<HTML
<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Mako updates</title>
<style>body{font:15px/1.5 system-ui,sans-serif;max-width:40rem;margin:3rem auto;padding:0 1rem}code{background:#8882;padding:2px 4px;border-radius:4px;word-break:break-all}</style>
<h1>Mako updates</h1>
<p>This is the apt repository for <a href="https://github.com/tommkotzu/mako">Mako</a>. Newest version: <b>$VERSION</b>.</p>
<p>Installing a Mako .deb from version 0.1.5 on sets this up by itself. New versions then arrive through Software Updater, or <code>sudo apt update &amp;&amp; sudo apt upgrade</code>.</p>
<p>Signing key fingerprint: <code>$FPR</code></p>
HTML

git init -q -b main
git add -A
git -c user.name="tommkotzu" -c user.email="tommkotzu@users.noreply.github.com" commit -q -m "Mako $VERSION"
gh repo view "$REPO" >/dev/null 2>&1 || gh repo create "$REPO" --public \
    --description "apt repository for Mako (https://github.com/tommkotzu/mako)" >/dev/null
git -c credential.helper= -c credential.helper="!gh auth git-credential" push -q --force "https://github.com/$REPO.git" main
gh api "repos/$REPO/pages" >/dev/null 2>&1 || \
    gh api -X POST "repos/$REPO/pages" -f "source[branch]=main" -f "source[path]=/" >/dev/null
echo "published $VERSION to https://tommkotzu.github.io/mako-apt/"
