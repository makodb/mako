cd $HOME 

curl -LO https://static.rust-lang.org/dist/rust-1.91.0-x86_64-unknown-linux-gnu.tar.gz

tar xzf rust-1.91.0-x86_64-unknown-linux-gnu.tar.gz

cd $HOME/rust-1.91.0-x86_64-unknown-linux-gnu/

mkdir -p $HOME/.local-rust

$HOME/rust-1.91.0-x86_64-unknown-linux-gnu/install.sh --prefix=$HOME/.local-rust


if [ -f "$HOME/.bashrc" ]; then
  echo '[ -d "$HOME/.local-rust/bin" ] && export PATH="$HOME/.local-rust/bin:$PATH"' >> "$HOME/.bashrc"
  source "$HOME/.bashrc"
elif [ -f "$HOME/.zshrc" ]; then
  echo '[ -d "$HOME/.local-rust/bin" ] && export PATH="$HOME/.local-rust/bin:$PATH"' >> "$HOME/.zshrc"
  source "$HOME/.zshrc"
fi

rustc --version
