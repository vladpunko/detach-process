# detach-process

![build](https://github.com/vladpunko/detach-process/actions/workflows/build.yml/badge.svg)

Spawn a new process and detach it from the current interactive session.

![usage-example](https://raw.githubusercontent.com/vladpunko/detach-process/master/detach.gif)

## Installation

Make sure the installed [gcc](https://gcc.gnu.org/install) compilers work without errors on the current operating system. To install this package as a standalone application with the command-line interface you are to run the following command:

```bash
sudo sh -c "$(curl https://raw.githubusercontent.com/vladpunko/detach-process/master/install.sh)"
```

You can also install this system package on your working machine (works for unix-like operating systems) from source code to `/usr/local/bin` as the standard system location for user's programs (this location can be changed at the user's discretion):

```bash
# Step -- 1.
git clone --depth=1 --branch=master https://github.com/vladpunko/detach-process.git

# Step -- 2.
cd ./detach-process/

# Step -- 3.
gcc -Wall -Wextra -Werror -Wno-unused-parameter detach.c -o detach

# Step -- 4.
sudo install -m 755 detach /usr/local/bin/detach
```

## Basic usage

Using this program allows you to create a new detached process avoiding complex shell pipelines in the current interactive session. It protects you from the trouble of closing an application which is the parent process of a different program.

```bash
detach ping -c 10 google.com  # pgrep ping | xargs ps -p
```

## Contributing

Pull requests are welcome.
Please open an issue first to discuss what should be changed.

Please make sure this system package works without errors.

```bash
# Step -- 1.
python3 -m venv .venv && source ./.venv/bin/activate && pip install cpplint pre-commit

# Step -- 2.
pre-commit install --config .githooks.yml

# Step -- 3.
cpplint detach.c
```

## License

[MIT](https://choosealicense.com/licenses/mit)
