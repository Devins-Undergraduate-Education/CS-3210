# Design of Lab 4's Login System

## Mechanisms for Securing

To secure our system, our implementation uses the following key components:
1. We utilize `init_hook` to create `/login.db` oon boot if it does not exist already. This ensures data is persistent and does not become overridden. Additionally, `init_hook` is used to both ensure only root owner has access to the file (other users are locked out) and inject the default `root/admin` account so there's always a privileged fallback user.
2. We ensure each user entry is stored as a fixed-size record with `uid`, `username`, a 16-bit salt, and a SHA-256 of `salt || password`. Additionally, this ensures password text is never written to the disk with increased resiliance to attacks (as per researched Linux security measures).
3. Thanks to the wonderful TA's and the made-available methods, we ensure records are encrypted with AES-256 with a key derived from a compile-time secret via SHA-256. With this, usernamed and salted hashes are hidden from unprivileged users interacting with the filesystem. 
4. As per advice of the TAs during lab, Salts are generated from a pseudo-random stream which uses `uptime`, `ticks`, and `PID`. This maintains uniqueness across all logs/records.
5. When a user logs in, the stored hash is recomputed and compared. In the event of a successful login, privileges to the user's UID are dropped before `exec`ing into the shell to ensure all subsequent commands are ran with the user's privilege.

## Files Created

1. We created a file named `/login.db` to serve as a binary database containing encrypted user records according to the securing mechanisms as described above. Each record stores approximately 80 bytes of logical data and is encrypted before persisting. Records are appended to the file, which maintains a persistent list of users. THe file at least contains the root entry and persists across reboots.
