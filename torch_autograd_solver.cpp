#include <torch/torch.h>

#include <iostream>

using at::Tensor;

// forked from pytorch 0.5
// https://github.com/sethah/pytorch/blob/81b61db9219ffeb8fc0c8ab3abe0f0b5a7edf4f4/tools/autograd/templates/Functions.cpp#L1514
// http://eprints.maths.ox.ac.uk/1079/1/NA-08-01.pdf
void symeig_backward(
    // backward variables
    Tensor gx,
    const Tensor& grad_loss_wrt_eigenvalues,
    const Tensor& grad_loss_wrt_eigenvectors,
    // forward variables
    const Tensor& x,
    const Tensor& eigenvalues,
    const Tensor& eigenvectors,
    // config
    bool upper) {
    auto vt = eigenvectors.t();
    if (grad_loss_wrt_eigenvectors.defined()) {
        Tensor F = eigenvalues.unsqueeze(0).expand_as(x).clone();
        F.sub_(at::unsqueeze(eigenvalues, 1));
        F.diagonal().fill_(INFINITY);
        F.pow_(-1);
        F.mul_(vt.mm(grad_loss_wrt_eigenvectors));
        gx.add_(eigenvectors.mm(F.mm(vt)));
    }
    if (grad_loss_wrt_eigenvalues.defined()) {
        gx.add_((eigenvectors * grad_loss_wrt_eigenvalues).mm(vt));
    }
    if (upper) {
        auto gxu = at::triu(gx.t(), 1);
        gx.triu_().add_(gxu);
    } else {
        auto gxl = at::tril(gx.t(), -1);
        gx.tril_().add_(gxl);
    }
}

std::tuple<Tensor, Tensor> batch_symeig_forward(const Tensor& input, bool upper)
{
    auto batch_size = input.size(0);
    auto n = input.size(1);
    auto w = at::empty({batch_size, n}, input.type());
    auto v = at::empty({batch_size, n, n}, input.type());
#pragma omp for
    for (int64_t i = 0; i < batch_size; ++i) {
        at::Tensor wi, vi;
        // FIXME use syev directly to avoid fragmented memory alloc https://github.com/pytorch/pytorch/blob/695465915a88f4803dfae152151bb56be5c99410/aten/src/TH/generic/THTensorLapack.cpp#L361
        std::tie(wi, vi) = at::symeig(input.slice(0, i, i + 1), true, upper);
        w.slice(0, i, i + 1).copy_(wi);
        v.slice(0, i, i + 1).copy_(vi);
    }
    return {w, v};
}

void batch_symeig_backward(
    // backward variables
    Tensor& grad_loss_wrt_input,
    const Tensor& grad_loss_wrt_eigenvalues,
    const Tensor& grad_loss_wrt_eigenvectors,
    // forward variables
    const Tensor& input,
    const Tensor& eigenvalues,
    const Tensor& eigenvectors,
    // config
    bool upper)
{
    auto batch_size = input.size(0);
#pragma omp for
    for (int64_t i = 0; i < batch_size; ++i) {
        symeig_backward(
            grad_loss_wrt_input.slice(0, i, i + 1),
            grad_loss_wrt_eigenvalues.slice(0, i, i + 1),
            grad_loss_wrt_eigenvectors.slice(0, i, i + 1),
            input.slice(0, i, i + 1),
            eigenvalues.slice(0, i, i + 1),
            eigenvectors.slice(0, i, i + 1),
            upper
            );
    }
}

void batch_symeig_backward_faster(
    // backward variables
    Tensor gx,
    const Tensor& grad_loss_wrt_eigenvalues,
    const Tensor& grad_loss_wrt_eigenvectors,
    // forward variables
    const Tensor& x,
    const Tensor& eigenvalues,
    const Tensor& eigenvectors,
    // config
    bool upper) {
    auto batch_size = x.size(0);
    auto vt = eigenvectors.transpose(-1, -2);
    if (grad_loss_wrt_eigenvectors.defined()) {
        // (b, m, m)
        Tensor F = eigenvalues.unsqueeze(-2).expand_as(x).clone();
        F.sub_(at::unsqueeze(eigenvalues, -2));
        // TODO implment batch_diagonal (strided [m * m, m + 1] vector ?)
#pragma omp for
        for (int64_t i = 0; i < batch_size; ++i) {
            F.slice(0, i, i + 1).diagonal().fill_(INFINITY);
        }
        F.pow_(-1);
        F.mul_(vt.bmm(grad_loss_wrt_eigenvectors));
        gx.add_(eigenvectors.mm(F.bmm(vt)));
    }
    if (grad_loss_wrt_eigenvalues.defined()) {
        gx.add_((eigenvectors * grad_loss_wrt_eigenvalues).bmm(vt));
    }

    if (upper) {
        // TODO implemnt batch_triu/l
#pragma omp for
        for (int64_t i = 0; i < batch_size; ++i) {
            auto&& gxi = gx.slice(0, i, i+1);
            auto gxu = at::triu(gxi.t(), 1);
            gxi.triu_().add_(gxu);
        }
    } else {
        for (int64_t i = 0; i < batch_size; ++i) {
            auto&& gxi = gx.slice(0, i, i+1);
            auto gxl = at::tril(gxi.t(), -1);
            gxi.tril_().add_(gxl);
        }
    }
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
    m.def("symeig_backward", &symeig_backward, "basic symeig backward");
    m.def("batch_symeig_forward", &batch_symeig_forward, "batch symeig forward");
    m.def("batch_symeig_backward", &batch_symeig_backward, "batch symeig backward");
    m.def("batch_symeig_backward_faster", &batch_symeig_backward_faster, "batch symeig backward");
}
