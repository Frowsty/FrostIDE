// SPDX-License-Identifier: Apache-2.0
// Linked file remains an evaluated source even though it is outside the project directory.
namespace EvaluatedSdk;
public static class Linked { public static string Message() => new Greeter().Greet("linked"); }
