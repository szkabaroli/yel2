# Written by tools/formula.sh for pre-alpha-1 (the release workflow): do not edit by hand.
class Yel < Formula
  desc "Compiler for the yel programming language"
  homepage "https://github.com/szkabaroli/yel2"
  version "pre-alpha-1"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-1/yel-darwin-arm64.tar.gz"
      sha256 "8b689a4aacd8693b2a69e00d554b749076ce9c17a172ada289557e62bc1b3745"
    end
    on_intel do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-1/yel-darwin-x64.tar.gz"
      sha256 "694729c56b85404b00638c6d83f7b0bd4617fecbbafe2b01e1b509d0e0ae6ce6"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-1/yel-linux-arm64.tar.gz"
      sha256 "c8124ac61c9fccb8cb0faff59f1e0f19444f888e7f32405b8654bb9334c8b722"
    end
    on_intel do
      url "https://github.com/szkabaroli/yel2/releases/download/pre-alpha-1/yel-linux-x64.tar.gz"
      sha256 "d0fb32b5da40ee1689417b269938379a90748c99228012381ca53e352639dacf"
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
