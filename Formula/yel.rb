# Written by tools/formula.sh for pre-alpha-2 (the release workflow): do not edit by hand.
class Yel < Formula
  desc "Compiler for the yel programming language"
  homepage "https://github.com/szkabaroli/yel2"
  version "pre-alpha-2"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-2/yel-darwin-arm64.tar.gz"
      sha256 "694025856b451bd81d68836f5debab6dd4be390f1b92d81238231d257fa5f439"
    end
    on_intel do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-2/yel-darwin-x64.tar.gz"
      sha256 "fc554f480b42d2fd898eb4575af4c826bc1200bb9b1dea807bed4e3339691358"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-2/yel-linux-arm64.tar.gz"
      sha256 "a2083548eb08482534654b5e4b38b5d37d5f8a4d8422b1754558a5e0e14ea663"
    end
    on_intel do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-2/yel-linux-x64.tar.gz"
      sha256 "761a18ea585429c3c042019c3c624c0696450132956b01ceb4215d5bc2c5fc09"
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
    (testpath/"hello.yel").write "main: func() { print(\"hello\\n\"); }\n"
    system bin/"yelc", "hello.yel", "hello.c"
    libs = %W[#{libexec}/build/yel.o #{libexec}/build/libuv.a -lm -lpthread]
    libs += %w[-ldl -lrt] if OS.linux?
    system ENV.cc, "-I#{libexec}/runtime", "-I#{libexec}/runtime/libuv/include", "hello.c", *libs, "-o", "hello"
    assert_equal "hello\n", shell_output("./hello")
  end
end
