#!/bin/sh
# The Homebrew formula of a release (Formula/yel.rb: this repository is its own tap), from the
# release's archives: each target's URL and SHA-256. The release workflow writes it after each
# release; by hand:
#
#   tools/formula.sh pre-alpha-3 out > Formula/yel.rb        (out: yel-<target>.tar.gz for each)
set -eu
tag=${1:?usage: tools/formula.sh <tag> <archives dir>}
archives=${2:?usage: tools/formula.sh <tag> <archives dir>}
repo=szkabaroli/yel2
base="https://github.com/$repo/releases/download/$tag"

sum() {
	file="$archives/yel-$1.tar.gz"
	[ -f "$file" ] || { echo "formula: no $file" >&2; exit 1; }
	if command -v sha256sum > /dev/null 2>&1; then sha256sum "$file" | cut -d' ' -f1; else shasum -a 256 "$file" | cut -d' ' -f1; fi
}

cat <<EOF
# Written by tools/formula.sh for $tag (the release workflow): do not edit by hand.
class Yel < Formula
  desc "Compiler for the yel programming language"
  homepage "https://github.com/$repo"
  version "$tag"
  license "MIT"

  on_macos do
    on_arm do
      url "$base/yel-darwin-arm64.tar.gz"
      sha256 "$(sum darwin-arm64)"
    end
    on_intel do
      url "$base/yel-darwin-x64.tar.gz"
      sha256 "$(sum darwin-x64)"
    end
  end

  on_linux do
    on_arm do
      url "$base/yel-linux-arm64.tar.gz"
      sha256 "$(sum linux-arm64)"
    end
    on_intel do
      url "$base/yel-linux-x64.tar.gz"
      sha256 "$(sum linux-x64)"
    end
  end

  def install
    # the archive whole (yelc reads its runtime beside it), and in bin/ a script for each tool
    # that names that home
    libexec.install Dir["*"]
    %w[yelc yel-lsp].each do |tool|
      (bin/tool).write_env_script libexec/"bin"/tool, YEL_HOME: libexec
    end
  end

  test do
    assert_match "yelc", shell_output("#{bin}/yelc --version")
    (testpath/"hello.yel").write "main: func() { print(\\"hello\\\\n\\"); }\\n"
    system bin/"yelc", "hello.yel", "hello.c"
    libs = %W[#{libexec}/build/yel.o #{libexec}/build/libuv.a -lm -lpthread]
    libs += %w[-ldl -lrt] if OS.linux?
    system ENV.cc, "-I#{libexec}/runtime", "-I#{libexec}/runtime/libuv/include", "hello.c", *libs, "-o", "hello"
    assert_equal "hello\\n", shell_output("./hello")
  end
end
EOF
