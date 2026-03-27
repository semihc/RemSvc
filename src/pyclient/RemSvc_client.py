#
# Python client for RemSvc
#

import argparse
import asyncio

import grpc
import RemSvc_pb2
import RemSvc_pb2_grpc


Server = None
Port = None

async def doPing(seq):
    endp = f"{Server}:{Port}"
    channel = grpc.insecure_channel(endp)
    stub = RemSvc_pb2_grpc.RemSvcStub(channel)
    ping = RemSvc_pb2.PingMsg(seq=seq)
    pong = stub.Ping(ping)
    print(f"Received:{pong} ")


async def doCmd(cmd):
    endp = f"{Server}:{Port}"
    channel = grpc.insecure_channel(endp)
    stub = RemSvc_pb2_grpc.RemSvcStub(channel)
    remcmd = RemSvc_pb2.RemCmdMsg()
    remcmd.cmd = cmd
    remres = stub.RemCmd(remcmd)
    print(f"Response:{remres} ")
    

async def main():
    global Server, Port
    
    parser = argparse.ArgumentParser(description="RemSvc client")
    parser.add_argument("--server", type=str, default="localhost", help="Server<str> def: %(default)s", required=False)
    parser.add_argument("--port", type=int, default=50051, help="Port<int> def: %(default)s", required=False)
    parser.add_argument("--ping", type=int, default=0, help="Ping<int> def: %(default)s", required=False)
    parser.add_argument("--cmd", type=str, default=None, help="Command<str>", required=False)
    args = parser.parse_args()

    Server = args.server
    Port = args.port

    if(args.ping): await doPing(args.ping)
    
    if(args.cmd): await doCmd(args.cmd)


    
if __name__ == '__main__':
    asyncio.run( main() )
    
