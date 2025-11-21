# rem-bash

A simple TCP server that accepts and executes simple bash commands.

## Motivation

In my day $JOB I most of my working by connecting via SSH to my work laptop to avoid having to switch between my
personal computer and work computer. However there are some pain points of exclusively working via SSH, and one of
them is not have the ability to open a browser via the terminal, either to authenticate with OIDC, open a GitHub
page using neovim vim-rhubarb or octo.nvim plugins, or some other use cases. So I created this small TCP server that
can run single-line bash commands by sending an small payload, terminated with a `\n` character.

## Running

You will need a C compiler to build rem-bash. The project is a single C file so compiling it should be pretty easy:

```bash
# You use gcc as well.
# Set -DBASH_PATH if you want to customize the location of there bash. Very useful if you're using nix.
clang server.c -O3 -o rem-bash -DBASH_PATH'"/bin/bash"'
```

Now you can run it and specify the host and port(only IPV4 is supported)

```bash
# Those are the defaults
./rem-bash --host 127.0.0.1 --port 1337
```

You can also use nix to build and run the rem-bash:
```bash
nix run .#default -- --host 127.0.0.1 --port 1337
```

Here's how you can send a command to it:
```bash
echo -n 'echo "Hello World!"\n' | nc localhost 1337
```

## Considerations

You should be **extremally careful** on how you use rem-bash. Exposing it over the network exposes a backdoor to your computer,
don't treat it lightly. I run it in my loopback interface and expose it to my SSH servers by remote forwarding with `ssh -R 1337:localhost:1337`,
but be careful if your SSH server enables the `GatewayPorts` as it can potentially allow anyone on the network or public
internet to run arbitrary commands on your machine! Also be careful if you share the server with other users. If that's the
case you can potentially forward rem-bash as an unix socket with the appropriates permissions(search for `StreamLocalBindMask`).

## Limitations

* Only tested on Linux. I _MIGHT_ work on other POSIX systems, but honestly I'm not sure if I'm strictly using POSIX APIs or not.
* Each request can execute a single command and the payload size cannot excedeed the value defined in `#define COMMAND_BUFFER_MAX_SIZE`.
* Multi-line bash commands are not supported, as the server excepts a `\n` character as its signal to stop reading data from the
connection and execute the command.
* The server will not reply anything. It could potentially pipe stdout and stderr as the response but I do not have a user case for it.

## Remarks

While I build this to solve a problem, I wanted to restrict myself to use C and learn how to build a server from scratch
only using POSIX/Linux APIs with no dependencies. You might see this project become a test-bed for trying different things, like
epoll, io_uring, pre-fork/thread work-stealing scheduler, even though the project would work just fine with using simpler APIs.

