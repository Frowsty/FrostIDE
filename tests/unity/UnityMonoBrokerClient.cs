// SPDX-License-Identifier: Apache-2.0
// Standalone Unity Mono transport fixture using the production current-user broker client.
using System;
using FrostIDE.UnityIntegration;

internal static class UnityMonoBrokerClient
{
    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("Expected project root and announcement arguments");
            return 2;
        }
        try
        {
            UnityBrokerClient.Announce(args[0], args[1]);
            Console.WriteLine("announcement accepted");
            return 0;
        }
        catch (UnityBrokerClient.BrokerFailure error)
        {
            // Request bodies include credentials and must stay out of fixture diagnostics.
            Console.Error.WriteLine("announcement refused at " + error.Stage + " (" + error.Kind + ")");
            return 1;
        }
    }
}
