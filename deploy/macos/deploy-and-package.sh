#!/bin/bash

# Run this from the project root, without arguments, or with the
# single argument --no-notarization to skip the notarize step

set -e

##!!! Make this accept more than one builddir for different architectures - lipo together the SV and library binaries, and include the helpers from the first builddir as primary and second, if present, as -translated suffix

usage() {
    echo
    echo "Usage: $0 [--no-notarization] <builddir> [<builddir> ...]"
    echo
    echo "  More than one <builddir> may be provided if multiple architectures"
    echo "  are to be merged."
    echo
    exit 2
}

notarize=yes
if [ "$1" = "--no-notarization" ]; then
    notarize=no
    shift
fi

builddirs="$@"
if [ -z "$builddirs" ]; then
    usage
fi

set -u

. deploy/metadata.sh

echo "Version: $version"

source="$full_name.app"
volume="$full_versioned"
volname="$full_name $version"
target="$volume"/"$full_name".app
dmg="$volume".dmg

if [ -d "$volume" ]; then
    echo "Target directory $volume already exists, not overwriting"
    exit 2
fi

if [ -f "$dmg" ]; then
    echo "Target disc image $dmg already exists, not overwriting"
    exit 2
fi

if [ "$notarize" = no ]; then
    echo
    echo "Note: The --no-notarization flag is set: won't be submitting for notarization"
fi

echo
echo "Making target tree."

mkdir "$volume" || exit 1

ln -s /Applications "$volume"/Applications
cp README.md "$volume/README.txt"
cp COPYING "$volume/COPYING.txt"
cp CITATION "$volume/CITATION.txt"

for builddir in $builddirs; do

    qtdir=$(grep qmake $builddir/meson-logs/meson-log.txt |
		grep -i 'found' |
		head -1 |
		sed 's/Found qmake: //' |
		sed 's/qmake[0-9]* found: YES (//' |
		sed 's,/bin/qmake.*,,')
    if [ -z "$qtdir" ]; then
	echo "Unable to discover QTDIR from build dir $builddir"
	exit 1
    elif [ ! -d "$qtdir" ]; then
	echo "Discovered QTDIR as $qtdir from build dir $builddir, but it doesn't exist"
	exit 1
    fi
    
    echo
    echo "(Re-)running deploy script in $builddir..."

    QTDIR="$qtdir" deploy/macos/deploy.sh "$builddir" || exit 1

    if [ ! -f "$target/Contents/Macos/$full_name" ]; then
	echo "App does not yet exist in target $target, copying verbatim..."
	cp -rp "$source" "$target"
    else
	echo "App exists in target $target, merging..."
	find "$source" -name "$full_name" -o -name \*.dylib -o -name Qt\* |
	    while read f; do
		this=$(lipo -archs "$f")
		that=$(lipo -archs "$volume/$f")
		if [ "$this" = "$that" ]; then
		    echo "File $f already has desired arch(s) in target"
		else
		    lipo "$f" "$volume/$f" -create -output "$volume/$f"
		fi
	    done
	for helper in vamp-plugin-load-checker piper-vamp-simple-server; do
	    path="Contents/MacOS/$helper"
	    if [ -f "$target/$path" ]; then
		arch=$(lipo -archs "$target/$path")
		mv "$target/$path" "$target/$path-$arch"
	    fi
	    arch=$(lipo -archs "$source/$path")
	    cp "$source/$path" "$target/$path-$arch"
	done
    fi
    echo "Done"

done

# update file timestamps so as to make the build date apparent
find "$volume" -exec touch \{\} \;

echo
echo "Code-signing volume..."

deploy/macos/sign.sh "$volume" || exit 1

    echo "Done"

    echo
    echo "Making dmg..."

    rm -f "$dmg"

    hdiutil create -srcfolder "$volume" "$dmg" -volname "$volname" -fs HFS+ && 
	rm -r "$volume"

    echo "Done"

    echo
    echo "Signing dmg..."

    gatekeeper_key="Developer ID Application: Particular Programs Ltd (73F996B92S)"

    codesign -s "$gatekeeper_key" -fv "$dmg"

    if [ "$notarize" = no ]; then
	echo
	echo "The --no-notarization flag was set: not submitting for notarization"
    else
	echo
	echo "Submitting dmg for notarization..."

	deploy/macos/notarize.sh "$dmg" || exit 1
    fi

    echo "Done"
