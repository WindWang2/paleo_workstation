#!/usr/bin/env python3
"""Validate an authenticated DoH answer before repairing an ephemeral CI host."""
import argparse
import ipaddress
import json
from pathlib import Path

HOSTS = {"archive.ubuntu.com", "security.ubuntu.com", "azure.archive.ubuntu.com"}


def entries(host, response):
    if host not in HOSTS:
        raise ValueError("host is outside the CI Ubuntu allowlist")
    if (type(response.get("Status")) is not int or response["Status"] != 0
            or response.get("TC") is True or response.get("CD") is not False):
        raise ValueError("DNS answer failed, was truncated, or disabled validation")
    questions = response.get("Question", [])
    if (len(questions) != 1 or questions[0].get("type") != 1
            or questions[0].get("name", "").rstrip(".").lower() != host):
        raise ValueError("DNS question does not match the requested A record")
    answers = response.get("Answer", [])
    names = {host}
    for _ in range(len(answers)):
        for record in answers:
            if record.get("type") == 5 and record.get("name", "").rstrip(".").lower() in names:
                names.add(record.get("data", "").rstrip(".").lower())
    addresses = []
    for record in answers:
        if record.get("type") != 1 or record.get("name", "").rstrip(".").lower() not in names:
            continue
        address = ipaddress.ip_address(record.get("data", ""))
        if (address.version != 4 or not address.is_global or address.is_multicast
                or address.is_reserved):
            raise ValueError("DNS returned a non-public IPv4 address")
        if str(address) not in addresses:
            addresses.append(str(address))
    if not addresses:
        raise ValueError("DNS answer has no usable A record")
    return "".join(f"{address}\t{host}\n" for address in addresses)


def selftest():
    import copy
    good = {"Status": 0, "CD": False, "TC": False,
            "Question": [{"name": "archive.ubuntu.com.", "type": 1}],
            "Answer": [{"name": "archive.ubuntu.com.", "type": 1, "data": "91.189.91.81"}]}
    assert entries("archive.ubuntu.com", good) == "91.189.91.81\tarchive.ubuntu.com\n"
    for field, value in [("Status", 2), ("CD", True), ("TC", True), ("Answer", [])]:
        bad = copy.deepcopy(good); bad[field] = value
        try:
            entries("archive.ubuntu.com", bad)
        except ValueError:
            continue
        raise AssertionError(f"accepted invalid {field}")
    for address in ["198.18.1.1", "127.0.0.1", "10.0.0.1", "224.0.0.1", "::1"]:
        bad = copy.deepcopy(good); bad["Answer"][0]["data"] = address
        try:
            entries("archive.ubuntu.com", bad)
        except ValueError:
            continue
        raise AssertionError(f"accepted invalid address {address}")
    bad = copy.deepcopy(good); bad["Question"][0]["name"] = "other.example"
    try:
        entries("archive.ubuntu.com", bad)
    except ValueError:
        pass
    else:
        raise AssertionError("accepted mismatching question")
    chain = copy.deepcopy(good)
    chain["Answer"] = [{"name": "archive.ubuntu.com.", "type": 5, "data": "mirror.example."},
                       {"name": "mirror.example.", "type": 1, "data": "91.189.91.81"}]
    assert entries("archive.ubuntu.com", chain) == entries("archive.ubuntu.com", good)
    print("DNS host validation: public A, CNAME, failures and unsafe addresses checked")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("host", nargs="?")
    parser.add_argument("response", nargs="?", type=Path)
    args = parser.parse_args()
    if args.selftest:
        selftest()
    else:
        if not args.host or not args.response:
            parser.error("host and response are required")
        raw = args.response.read_bytes()
        if len(raw) > 65536:
            raise ValueError("DNS response exceeds the bounded input size")
        print(entries(args.host, json.loads(raw)), end="")
