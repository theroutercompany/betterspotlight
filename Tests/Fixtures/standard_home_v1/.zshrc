# ~/.zshrc for BetterSpotlight development
export PATH="$HOME/.local/bin:$PATH"
export PATH="$HOME/go/bin:$PATH"

alias ll='ls -lah'
alias gs='git status -sb'
alias bsbuild='swift build'
alias bstest='swift test'

source ~/.nvm/nvm.sh

# Prompt and shell options
setopt autocd
setopt hist_ignore_dups
HISTSIZE=50000
SAVEHIST=50000
# note line 1
# note line 2
# note line 3
# note line 4
