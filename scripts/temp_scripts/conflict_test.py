import numpy as np
from tqdm import tqdm

# Global variables for ranks and probabilities
ranks = None
probabilities = None

def generate_zipf_samples(sample_size):
    """
    Generates Zipf-like samples based on precomputed ranks and probabilities.
    
    Args:
        sample_size (int): Number of samples to generate.
        
    Returns:
        np.array: Array of sampled numbers.
    """
    # Generate samples using np.random.choice with precomputed probabilities
    return np.random.choice(ranks, size=sample_size, p=probabilities)

def calculate_conflict_rate(key_range, zipf_coeff, k, iterations=1000):
    """
    Calculates the conflict rate for sampling k numbers from a Zipf distribution.

    Args:
        key_range (int): The upper bound of the key range (1 to key_range).
        zipf_coeff (float): The Zipf distribution coefficient.
        k (int): Number of numbers to sample in each iteration.
        iterations (int): Number of iterations to run (default: 1000).

    Returns:
        float: The conflict rate as a percentage.
    """
    global ranks, probabilities
    
    # Precompute ranks, weights, and probabilities
    ranks = np.arange(1, key_range + 1)
    weights = (ranks + 1) ** -zipf_coeff
    probabilities = weights / np.sum(weights)

    conflict_count = 0

    # Use tqdm to show progress bar for iterations
    for _ in tqdm(range(iterations), desc=f"Calculating Conflict Rate for k={k}", ncols=100):
        # Generate k samples
        samples = generate_zipf_samples(k)

        # Efficiently check for duplicates using np.bincount
        if np.any(np.bincount(samples) > 1):  # If any sample appears more than once
            conflict_count += 1

    # Calculate the conflict rate
    conflict_rate = (conflict_count / iterations) * 100
    return conflict_rate

if __name__ == "__main__":
    # Input values
    key_range = int(input("Enter the key range: "))
    zipf_coeff = float(input("Enter the Zipf coefficient: "))
    k_start = int(input("Enter the starting value of k: "))
    k_end = int(input("Enter the ending value of k: "))
    
    # Calculate and output conflict rates for every k in the range [k_start, k_end]
    for k in range(k_start, k_end + 1):
        conflict_rate = calculate_conflict_rate(key_range, zipf_coeff, k)
        print(f"Conflict rate for k={k}: {conflict_rate:.2f}%")

