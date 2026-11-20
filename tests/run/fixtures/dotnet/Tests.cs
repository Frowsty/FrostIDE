// SPDX-License-Identifier: Apache-2.0
using Microsoft.VisualStudio.TestTools.UnitTesting;
namespace FrostFixture;
[TestClass]
public sealed class Tests
{
    [TestMethod] public void Passes() { Assert.AreEqual(2, 1 + 1); }
    [TestMethod] public void Fails() { Assert.Fail("Intentional adapter failure"); }
    [TestMethod, Ignore("Intentional adapter skip")] public void Skips() { Assert.Fail("Must not execute"); }
}
