#!/usr/bin/env python3
#
# Python client for RemSvc
#

import argparse
import asyncio
import zlib

import grpc
import RemSvc_pb2
import RemSvc_pb2_grpc


DEFAULT_PORT = 50051


def crc32Hex(data: str) -> str:
    return format(zlib.crc32(data.encode("utf-8")) & 0xFFFFFFFF, "08x")


def buildChannel(server: str, port: int, tls: bool, ca_cert: str = None):
    target = f"{server}:{port}"
    if tls:
        root_certs = None
        if ca_cert:
            with open(ca_cert, "rb") as f:
                root_certs = f.read()
        credentials = grpc.ssl_channel_credentials(root_certificates=root_certs)
        return grpc.aio.secure_channel(target, credentials)
    return grpc.aio.insecure_channel(target)


async def doPing(stub: RemSvc_pb2_grpc.RemSvcStub, seq: int):
    request = RemSvc_pb2.PingMsg(seq=seq)
    response = await stub.Ping(request, timeout=30.0)
    print(f"Ping response: seq={response.seq} time={response.time} data={response.data}")


async def doGetStatus(stub: RemSvc_pb2_grpc.RemSvcStub):
    request = RemSvc_pb2.Empty()
    response = await stub.GetStatus(request, timeout=30.0)
    print(f"Status response: rc={response.rc} msg={response.msg}")


async def doRemCmd(stub: RemSvc_pb2_grpc.RemSvcStub,
                 cmd: str,
                 cmdtyp: int,
                 tid: int,
                 cmdusr: str):
    request = RemSvc_pb2.RemCmdMsg(
        cmd=cmd,
        cmdtyp=cmdtyp,
        tid=tid,
        hsh=crc32Hex(cmd),
    )
    if cmdusr:
        request.cmdusr = cmdusr

    response = await stub.RemCmd(request, timeout=30.0)
    print(f"Response: rc={response.rc}")
    if response.out:
        print(f"stdout:\n{response.out}")
    if response.err:
        print(f"stderr:\n{response.err}")


async def doCmdStream(stub: RemSvc_pb2_grpc.RemSvcStub,
                        cmds: list[str],
                        cmdtyp: int,
                        cmdusr: str):
    stream = stub.RemCmdStrm()

    async def readResponses():
        try:
            async for response in stream:
                print(f"Response: tid={response.tid} rc={response.rc}")
                if response.out:
                    print(f"stdout:\n{response.out}")
                if response.err:
                    print(f"stderr:\n{response.err}")
        except grpc.aio.AioRpcError as exc:
            print(f"Stream error: {exc.code()}: {exc.details()}")

    reader_task = asyncio.create_task(readResponses())

    for index, cmd in enumerate(cmds, start=1):
        request = RemSvc_pb2.RemCmdMsg(
            cmd=cmd,
            cmdtyp=cmdtyp,
            tid=index,
            hsh=crc32Hex(cmd),
        )
        if cmdusr:
            request.cmdusr = cmdusr
        await stream.write(request)

    await stream.done_writing()
    await reader_task


async def main():
    parser = argparse.ArgumentParser(description="RemSvc client")
    parser.add_argument("--server", type=str, default="localhost",
                        help="Server hostname (default: %(default)s)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help="Server port (default: %(default)s)")
    parser.add_argument("--ping", type=int, default=0,
                        help="Ping sequence number")
    parser.add_argument("--status", action="store_true",
                        help="Query server status")
    parser.add_argument("--cmd", action="append",
                        help="Command to execute; repeatable for multiple commands")
    parser.add_argument("--cmdtyp", type=int, default=0,
                        help="Command type (0=native shell, 1=PowerShell)")
    parser.add_argument("--tid", type=int, default=0,
                        help="Transaction id for unary command")
    parser.add_argument("--user", type=str, default="",
                        help="OS user to execute the command as (Linux only)")
    parser.add_argument("--stream", action="store_true",
                        help="Use bidirectional streaming RemCmdStrm")
    parser.add_argument("--tls", action="store_true",
                        help="Connect using TLS")
    parser.add_argument("--ca-cert", type=str, default="",
                        help="CA certificate PEM file for TLS")
    args = parser.parse_args()

    channel = buildChannel(args.server, args.port, args.tls, args.ca_cert or None)
    stub = RemSvc_pb2_grpc.RemSvcStub(channel)

    if args.ping:
        await doPing(stub, args.ping)

    if args.status:
        await doGetStatus(stub)

    if args.cmd:
        if args.stream:
            await doCmdStream(stub, args.cmd, args.cmdtyp, args.user)
        else:
            for index, cmd in enumerate(args.cmd, start=1):
                tid = args.tid if args.tid else index
                await doRemCmd(stub, cmd, args.cmdtyp, tid, args.user)

    await channel.close()


if __name__ == '__main__':
    asyncio.run(main())
    
