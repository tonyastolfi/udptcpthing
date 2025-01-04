# udptcpthing - Tunnelling Proxy for Self-Hosted Minecraft

This is a quick proxy utility I wrote so that my kids could play Minecraft on a private server with their cousins.  It works by using a spare computer at your house (behind your Internet router) to run Minecraft Bedrock server, and a cheap cloud VM with a static public IP address to tunnel traffic over ssh.

The basic idea is:

1. Download/clone this repo (it includes x86_64 Linux binaries) on the machine you'll be running the Minecraft server
2. Create (or repurpose) a cheap, tiny VM with a static IP; I suggest using Linode, they have a shared CPU instance type for just $5/month.
  - Choose a region that is close to all the people who will be playing, to make sure latencies are low
  - Select Ubuntu 22 Linux as the VM image **Make sure you get this right or `udptcpthing` will NOT work!**
  - **Check the box that says Private IP**
  - Upload/create your ssh keys, etc.
3. Install Minecraft Bedrock Server on the machine from step 1
4. Edit `~/.ssh/config` on the local server to add a shortcut for the cloud VM:
  ```
  Host vm-proxy
      User root
      HostName YOUR_SERVERS_IP_ADDRESS
      IdentityFile PATH_TO_YOUR_SSH_KEY_FILE
  ```
  Make sure this is working by running (this will ask you about the remote host fingerprint first time):
  ```
  ssh vm-proxy
  ```
5. Use scp to upload the `udptcpthing` release to your Linode (or other) VM:
  ```
  scp releases/v0.1/udptcpthing-0.1-x86_64-linux.tar.gz vm-proxy:/root/
  ```
6. Un-tar `udptcpthing-0.1-x86_64-linux.tar.gz` on the local and remote servers:
  ```
  cd releases/v0.1
  tar xvf udptcpthing-0.1-x86_64-linux.tar.gz
  ssh vm-proxy "tar xvf udptcpthing-0.1-x86_64-linux.tar.gz"
  ```
7. Start `udptcpthing` on the local server:
  ```
  MODE=tcp2udp ./udptcpthing
  ```
8. In a different terminal, on the local server, run the public-facing `udptcpthing` proxy via ssh:
  ```
  ssh -R 7777:127.0.0.1:7777 vm-proxy "MODE=udp2tcp ./udptcpthing"
  ```
9. In a third terminal, on the local server, run Minecraft Bedrock Server (see the official config guide for how to download and set this up; make sure you secure things... this is on you, not my fault if you leave a gaping security hole in your home network!)

Now in regular Minecraft (Bedrock), hit Play, go to Servers, go to the bottom, say Add/New, give it a name, and enter the static IP of your cloud VM from step 2 (leave the port as the default).

You should now be able to connect from anywhere on the internet!  Enjoy, and happy Minecrafting!
