//! yel2 in Zed: starts yel-lsp (tools/lsp), the compiler's errors as diagnostics and formatting as
//! yelc --fmt does. The highlighting, outline and indentation are the grammar's (editors/tree-sitter-yel).

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

struct YelExtension;

impl zed::Extension for YelExtension {
    fn new() -> Self {
        YelExtension
    }

    /// yel-lsp's command: the binary the settings name (lsp.yel-lsp.binary.path), else yel-lsp on
    /// the PATH, else the worktree's build/yel-lsp; its argument the runtime's prelude, the
    /// settings' (lsp.yel-lsp.binary.arguments), else the worktree's runtime/prelude.yel where it
    /// has one (the yel2 repository), else none (yel-lsp's default)
    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let binary = LspSettings::for_worktree(language_server_id.as_ref(), worktree)
            .ok()
            .and_then(|settings| settings.binary);
        let root = worktree.root_path();
        let command = binary
            .as_ref()
            .and_then(|binary| binary.path.clone())
            .or_else(|| worktree.which("yel-lsp"))
            .unwrap_or_else(|| format!("{root}/build/yel-lsp"));
        let args = match binary.and_then(|binary| binary.arguments) {
            Some(arguments) => arguments,
            None if worktree.read_text_file("runtime/prelude.yel").is_ok() => {
                vec![format!("{root}/runtime/prelude.yel")]
            }
            None => Vec::new(),
        };
        Ok(zed::Command {
            command,
            args,
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(YelExtension);
