﻿// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Runtime.Intrinsics;

namespace System.Numerics.Tensors
{
    public static partial class TensorPrimitives
    {
        /// <summary>Computes the element-wise negation of each number in the specified tensor.</summary>
        /// <param name="x">The tensor, represented as a span.</param>
        /// <param name="destination">The destination tensor, represented as a span.</param>
        /// <exception cref="ArgumentException">Destination is too short.</exception>
        /// <exception cref="ArgumentException"><paramref name="x"/> and <paramref name="destination"/> reference overlapping memory locations and do not begin at the same location.</exception>
        /// <remarks>
        /// <para>
        /// This method effectively computes <c><paramref name="destination" />[i] = -<paramref name="x" />[i]</c>.
        /// </para>
        /// <para>
        /// If any of the element-wise input values is equal to <see cref="IFloatingPointIeee754{TSelf}.NaN"/>, the resulting element-wise value is also NaN.
        /// </para>
        /// </remarks>
        public static void Negate<T>(ReadOnlySpan<T> x, Span<T> destination)
            where T : IUnaryNegationOperators<T, T>
        {
            if (typeof(T) == typeof(Half) && TryUnaryInvokeHalfAsInt16<T, NegateOperator<float>>(x, destination))
            {
                return;
            }

            InvokeSpanIntoSpan<T, NegateOperator<T>>(x, destination);
        }

        /// <summary>-x</summary>
        internal readonly struct NegateOperator<T> : IUnaryOperator<T, T> where T : IUnaryNegationOperators<T, T>
        {
            public static bool Vectorizable => true;
            public static T Invoke(T x) => -x;
            public static Vector128<T> Invoke(Vector128<T> x) => -x;
            public static Vector256<T> Invoke(Vector256<T> x) => -x;
            public static Vector512<T> Invoke(Vector512<T> x) => -x;
        }
    }
}
