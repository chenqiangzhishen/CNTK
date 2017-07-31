//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

// experimental/prototypical layers lib in C++

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "CNTKLibrary.h"
#include "Common.h"

#include <functional>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#define let const auto

using namespace CNTK;
using namespace std;

namespace Dynamite {

    // slice the last dimension if an NDArrayView (index with index i; then drop the axis)
    // This is used for MB conversion.
    static NDArrayViewPtr Index(NDArrayViewPtr data, size_t i)
    {
        auto dims = data->Shape().Dimensions();
        auto startOffset = vector<size_t>(dims.size(), 0);
        auto extent = dims;
        if (startOffset.back() != i || extent.back() != 1)
        {
            startOffset.back() = i;
            extent.pop_back(); // missing extend values default to 1 but do not generate an output axis
            data = data->SliceView(startOffset, extent, true); // slice it
            dims = data->Shape().Dimensions();
        }
        else
        {
            let newShape = NDShape(vector<size_t>(dims.begin(), dims.end() - 1));
            data = data->AsShape(newShape); // and drop the final dimension
        }
        return data;
    }

    // helper for converting data to dense
    template<typename ElementType>
    static NDArrayViewPtr MakeEye(size_t n, const CNTK::DataType& dataType, const CNTK::DeviceDescriptor& device)
    {
        vector<ElementType> buffer(n*n, 0);
        for (size_t i = 0; i < n; i++)
            buffer[i*n + i] = 1;
        auto eye = make_shared<NDArrayView>(dataType, NDShape{ n, n }, buffer.data(), buffer.size() * sizeof(ElementType), DeviceDescriptor::CPUDevice(), /*readOnly=*/false);
        eye = eye->DeepClone(device);
        return eye;
    }
    static NDArrayViewPtr Eye(size_t n, const CNTK::DataType& dataType, const CNTK::DeviceDescriptor& device)
    {
        static map<pair<size_t, CNTK::DataType>, NDArrayViewPtr> cached;
        let key = make_pair(n, dataType);
        auto iter = cached.find(key);
        if (iter != cached.end())
            return iter->second;
        // need to create it
        NDArrayViewPtr eye;  device;
        switch (dataType)
        {
        case DataType::Float:  eye = MakeEye<float>(n, dataType, device);  break;
        case DataType::Double: eye = MakeEye<double>(n, dataType, device); break;
        default: throw logic_error("Eye: Unsupported data type.");
        }
        cached[key] = eye;
        return eye;
    }

    // returns vector[numArgs] OF vector[numBatchItems] OF Constant[seqLen,sampleShape]
    // or no seqLen if isSequence is false for the respective stream
    static void FromCNTKMB(vector<vector<Variable>>& res, const vector<ValuePtr>& inputs, const vector<bool>& isSequence, const DeviceDescriptor& device) // variables needed for axis info only
    {
        let numArgs = inputs.size();
        res.clear(); // free memory in case it is still held
        res.resize(numArgs);
        size_t numSeq = 0;
        for (size_t i = 0; i < numArgs; i++)
        {
            // prepare argument i
            let& input = inputs[i];
            // UnpackVariableValue() requires an InputVariable for reference, so create one
            // CNTK Readers always return data with 2 axes (length, batch), even for data without sequence axis (the readers don't know).
            // Hence, users must pass in whether a stream is meant to have a sequence axis or not.
            let fullShape = input->Shape();
            let sampleShape = fullShape.SubShape(0, fullShape.Rank() - 2); // input.Shape() includes the dynamic axes shape at the end
            let dynamicAxes = isSequence[i] ? Axis::DefaultInputVariableDynamicAxes() : vector<Axis>{ Axis::DefaultBatchAxis() };
            let variable = CNTK::InputVariable(sampleShape, input->IsSparse(), input->GetDataType(), dynamicAxes);
            auto sequences = input->UnpackVariableValue(variable, device); // -> vector[numBatchItems] of NDArrayViews
            if (numSeq == 0)
                numSeq = sequences.size();
            else if (numSeq != sequences.size())
                CNTK::LogicError("FromCNTKMB: Streams must all have the same number of sequences.");
            auto hasAxis = variable.DynamicAxes().size() > 1;

            auto& arg = res[i];
            arg.resize(numSeq);   // resulting argument
            for (size_t s = 0; s < numSeq; s++)
            {
                auto data = sequences[s];      // NDArrayView
                // return in correct shape
                if (!hasAxis)
                {
                    if (data->Shape().Dimensions().back() != 1)
                        CNTK::LogicError("FromCNTKMB: Streams declared as !isSequence must have a trailing dimension of 1.");
                    data = Index(data, 0); // slice off sample axis (the last in C++)
                }
#if 1 // needed for now since PlainTextDeserializer cannot deliver Dense data, and Dynamite metric blows up on Sparse
                if (data->IsSparse())
                {
                    // multiply with  an identity matrix
                    auto eye = Eye(data->Shape()[0], data->GetDataType(), data->Device());
                    data = NDArrayView::MatrixProduct(false, eye, false, data, false, 1.0, 1);
                }
#endif
                arg[s] = Constant(data); // TODO: does this make a copy?
                //data = data->AsShape(data->Shape()); // (for debugging)
            }
        }
    }

}; // namespace
