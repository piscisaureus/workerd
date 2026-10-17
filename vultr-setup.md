# Vultr build server: bert

An x86-64 AMD bare metal machine used to build and test the guest-kernel
isolation work described in `security-idea-2.md`. The design is x86 specific
and needs KVM, MPK and PCID on real hardware, none of which an ARM laptop can
provide.

## Machine

| Item | Value |
| --- | --- |
| Label / hostname | `bert` |
| Vultr id | `491b17cc-144b-432e-bf79-6ca99793c3aa` |
| Region | Chicago (`ord`) |
| Plan | `vbm-24c-384gb-amd5` ($0.73/hour, listed $995/month) |
| CPU | AMD EPYC 9255, Zen 5, 24 cores / 48 threads |
| RAM / disk | 384 GB / 2 x 480 GB NVMe |
| OS | Ubuntu 24.04 LTS (Vultr os id 2284) |
| IPv4 | `45.76.229.177` |
| IPv6 | `2001:19f0:5c01:0e61:6e92:cfff:fe37:03c0` |
| DNS | `bert.deno.co` (A + AAAA, TTL 300), still pointing at the previous IP |

The first instance was `vbm-24c-256gb-amd` (EPYC 7443P, Zen 3, 24c/48t,
256 GB, 2 x 480 GB SSD, $725/month) in `lax`, Vultr id
`4205fcba-1633-4df5-8899-f15be9be16f8`, at `45.32.95.240` /
`2001:19f0:6000:2cb0:5e6f:69ff:fe57:5c80`. It was deleted on 2026-10-13.
Every published guest-kernel performance number was measured on it, so
results from the Zen 5 machine need re-baselining before being compared.
The Zen 3 24-core plan is out of stock in every US region, which is why the
replacement is a different generation; `GET
/v2/regions/{region}/availability?type=vbm` lists what is actually in stock,
and the `locations` field of `/v2/plans-metal` only says where a plan
exists.

CPU flags relevant to the project, confirmed present: `pku`, `ospke`, `pcid`,
`invpcid`, `svm`, `npt`. `/dev/kvm` is owned by `root:kvm` with mode 0660.

## What is on the server

Hardware as reported by the OS:

- CPU: one socket, 24 cores, 48 threads, one NUMA node, 4 x 32 MiB L3.
- Memory: 251 GiB usable.
- KVM: `kvm_amd` loaded with `npt=1` and `nested=1`. `systemd-detect-virt`
  reports `none`, so this is real hardware, not a VM.
- Network: `eno12399np0` carries both public addresses (`45.32.95.240/22`
  and the IPv6 `/64`). A second port `eno12409np1` is present but down.

Disks:

| Device | Size | Type | Use |
| --- | --- | --- | --- |
| `/dev/sdb` | 447 GiB | SATA SSD (SK hynix HFS480G3H2X069N) | root: `sdb1` EFI, `sdb2` ext4 `/`, plus a swapfile |
| `/dev/sda` | 447 GiB | SATA SSD (same model) | blank, unpartitioned |
| `/dev/nvme0n1` | 1.75 TiB | NVMe U.2 (Dell Ent NVMe v2 AGN RI 1.92 TB) | blank, unpartitioned |
| `/dev/nvme1n1` | 1.75 TiB | NVMe U.2 (same model) | blank, unpartitioned |

The OS lives on one of the two SATA SSDs. The two NVMe drives each carry
one ext4 filesystem, labelled `nvme0` and `nvme1`, mounted by label through
`/etc/fstab` at `/nvme0` and `/nvme1` with `noatime,nofail`. Both mount
points are owned by `piscisaureus`. `/nvme1` is unused.

Layout on `/nvme0`:

- `/nvme0/work`: source checkouts. `~/work` is a symlink to it, and the
  workerd clone is at `/nvme0/work/workerd` with `origin` pointing at
  `cloudflare/workerd` over HTTPS and `fork` at
  `git@github.com:piscisaureus/workerd.git`. The server has no GitHub SSH
  key; push through agent forwarding (`ssh -A`) from a machine that has
  one.
- `/nvme0/bazel`: bazel state, selected by `~/.bazelrc`, which also
  selects the LLVM 22 toolchain:

```
startup --output_user_root=/nvme0/bazel/output
build --disk_cache=/nvme0/bazel/disk-cache
build --repository_cache=/nvme0/bazel/repo-cache
build:linux --repo_env=CC=/usr/lib/llvm-22/bin/clang
build:linux --linkopt=--ld-path=/usr/lib/llvm-22/bin/ld.lld
build:linux --host_linkopt=--ld-path=/usr/lib/llvm-22/bin/ld.lld
build:linux --repo_env=AR=/usr/lib/llvm-22/bin/llvm-ar
```

Builds run inside `tmux` (session `build`) so they survive a dropped SSH
session, and log to `/nvme0/work/build-N.log`.

The plan originally wanted was `vbm-24c-384gb-amd` (EPYC 9254, Zen 4) in
Silicon Valley, but that plan is not stocked on the US West Coast. Zen 3 is
the minimum for the project because it is the first AMD generation with both
PCID and MPK, so the 7443P is sufficient. Availability changes constantly;
`GET /v2/regions/{region}/availability?type=vbm` lists what is in stock.

## Access

```
ssh piscisaureus@bert.deno.co
```

- User `piscisaureus`, member of `sudo` and `kvm`, passwordless sudo via
  `/etc/sudoers.d/piscisaureus`.
- Authorized key: the local `~/.ssh/id_rsa.pub`, which is also the key
  registered with Vultr under the name `piscisaureus`
  (`b0b02b2b-c818-4522-8b8e-86f1c08a5f61`).
- `root` accepts the same key. Vultr also sets a root password, visible in
  the Vultr console.

## Provisioning

The Vultr API key is in `.env.sh` (`VULTR_API_KEY`), which is excluded from
git through `.git/info/exclude`. The server was created with:

```
source .env.sh
curl -s -X POST \
  -H "Authorization: Bearer $VULTR_API_KEY" \
  -H "Content-Type: application/json" \
  https://api.vultr.com/v2/bare-metals \
  -d '{
    "region": "ord",
    "plan": "vbm-24c-384gb-amd5",
    "os_id": 2284,
    "label": "bert",
    "hostname": "bert",
    "sshkey_id": ["b0b02b2b-c818-4522-8b8e-86f1c08a5f61"],
    "enable_ipv6": true,
    "tags": ["workerd"]
  }'
```

Bare metal installs take 5 to 30 minutes. Poll
`GET /v2/bare-metals/{id}` until `status` is `active`; the IP addresses
appear before that. SSH answers within a minute or two of `active`.

Vultr accepts `DELETE /v2/bare-metals/{id}` while a server is still
`pending` and returns 204, but the deletion is queued and only runs once the
install finishes. A pending server therefore keeps showing up in listings
for a while after it has been deleted.

## Post-install setup

Run as root right after the first login. `PUBKEY` is the contents of the
local `~/.ssh/id_rsa.pub`.

```bash
#!/bin/bash
set -euo pipefail
PUBKEY="$1"
if ! id piscisaureus >/dev/null 2>&1; then
  useradd -m -s /bin/bash -G sudo,kvm piscisaureus
fi
usermod -aG kvm piscisaureus || true
echo 'piscisaureus ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/piscisaureus
chmod 440 /etc/sudoers.d/piscisaureus
install -d -m 700 -o piscisaureus -g piscisaureus /home/piscisaureus/.ssh
echo "$PUBKEY" > /home/piscisaureus/.ssh/authorized_keys
chown piscisaureus:piscisaureus /home/piscisaureus/.ssh/authorized_keys
chmod 600 /home/piscisaureus/.ssh/authorized_keys

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q git curl build-essential clang lld python3 python3-pip \
  unzip zip pkg-config libc++-dev libc++abi-dev tmux htop jq

# bazelisk, as "bazel"
curl -fsSL -o /usr/local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel

# just
curl -fsSL https://just.systems/install.sh | bash -s -- --to /usr/local/bin

# node + pnpm for the TypeScript parts of the workerd build
curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
apt-get install -y -q nodejs
npm install -g pnpm
```

Invocation from the laptop:

```
scp setup-root.sh root@45.76.229.177:/root/
ssh root@45.76.229.177 "bash /root/setup-root.sh '$(cat ~/.ssh/id_rsa.pub)'"
```

Installed versions at setup time: bazel (bazelisk) 9.2.0, just 1.58.0,
node 22, pnpm 12.

workerd needs LLVM 22 or newer (`docs/development.md`); Ubuntu 24.04's own
clang 18 fails on the first `#embed` use with
`unknown argument: '--embed-dir=...'`. LLVM 22 comes from apt.llvm.org, the
same way CI installs it in `.github/actions/setup-runner/action.yml`:

```
wget https://apt.llvm.org/llvm.sh
sed -i '/apt-get install/d' llvm.sh
sudo ./llvm.sh 22
sudo apt-get install -y --no-install-recommends clang-22 lld-22 \
  libunwind-22-dev libc++-22-dev libclang-rt-22-dev llvm-22
for t in llvm-symbolizer llvm-profdata llvm-cov; do
  sudo ln -sf /usr/bin/$t-22 /usr/bin/$t
done
```

Bazel is pointed at it through `~/.bazelrc` (see the disk layout section)
rather than the repo's `.bazelrc`, so the checkout stays clean.

## DNS

The `deno.co` zone is `deno-co` in GCP project `misc-dns`. `bert.deno.co`
used to be a CNAME to `alias.dev.deno.co`; a CNAME cannot coexist with other
records, so it was removed and replaced with A and AAAA records:

```
P="--project misc-dns --zone deno-co"
gcloud dns record-sets transaction start $P
gcloud dns record-sets transaction remove $P --name bert.deno.co. \
  --type CNAME --ttl 300 'alias.dev.deno.co.'
gcloud dns record-sets transaction add $P --name bert.deno.co. \
  --type A --ttl 300 45.32.95.240
gcloud dns record-sets transaction add $P --name bert.deno.co. \
  --type AAAA --ttl 300 2001:19f0:6000:2cb0:5e6f:69ff:fe57:5c80
gcloud dns record-sets transaction execute $P
```

Because the old CNAME target answers SSH on the same host key as several
other `*.dev.deno.co` names, a stale resolver cache sends `ssh
bert.deno.co` to the wrong machine and the login fails with "Permission
denied (keyboard-interactive)". Wait for the 300 second TTL to expire or
connect by IP. Any machine that connected to the name while it still
pointed at the old target has the wrong host key stored for it; clear it
with `ssh-keygen -R bert.deno.co` before connecting again.

`gcloud` needs a valid login for `bert@deno.com`. On a machine without a
browser, `gcloud auth login --no-launch-browser` prints a URL; open it
elsewhere and paste the verification code back.
